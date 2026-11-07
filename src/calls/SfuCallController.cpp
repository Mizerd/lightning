#include "calls/SfuCallController.h"

#include "calls/WindowCaptureSrc.h"

#include <QLoggingCategory>

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#if defined(Q_OS_LINUX) && defined(LIGHTNING_HAVE_QPA_SCREEN)
// The only way to get a screen's native, root-relative rectangle, which is
// what the X11 capture element addresses. Private Qt API: no public accessor
// exposes it and the arithmetic alternative is wrong (see nativeScreenRect()
// in the header). Linux-only and gated on the CMake probe; without it the
// fallback refuses rather than guessing.
#include <qpa/qplatformscreen.h>
#endif
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRandomGenerator>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "calls/CallParticipantModel.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#include "calls/CameraPortal.h"
#include "calls/RtcController.h"
#include "calls/ScreenCastPortal.h"
#include "calls/OneToOneDm.h"
#include "calls/SfuMediaEngine.h"
#include "calls/noise/MicProcessing.h"
#include "app/SandboxEnvironment.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
#include "calls/CallFrameCryptor.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include <QVideoFrame>
#include <QVideoSink>

#include "calls/SfuVideoRouter.h"
#endif
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
#include <QImage>
#include <QVideoFrame>
#include <QVideoSink>
#endif

Q_LOGGING_CATEGORY(lcSfuCall, "lightning.calls.group")

namespace {
// Element caps a notification's lifetime at 90 s and the Rust side clamps to
// the same value.
constexpr quint64 kAnnounceLifetimeMs = 90000;
}

namespace {
/// Closes a descriptor the desktop portal handed us (it is ours, so every
/// path out, refusals included, must close it). Compiles everywhere: there is
/// no portal off Unix and `::close` is undeclared on MinGW.
void closePortalFd(int fd)
{
#ifdef Q_OS_UNIX
    if (fd >= 0)
        ::close(fd);
#else
    Q_UNUSED(fd);
#endif
}

#ifdef HAVE_LIGHTNING_WEBRTC

bool runningSandboxed()
{
#ifdef Q_OS_LINUX
    return sandboxenv::isSandboxed();
#else
    return false;
#endif
}

bool v4l2DeviceNodeVisible()
{
#ifdef Q_OS_LINUX
    QDir dev(QStringLiteral("/dev"));
    return !dev.entryList(QStringList{ QStringLiteral("video*") },
                          QDir::System | QDir::Files | QDir::Dirs
                              | QDir::NoDotAndDotDot)
                .isEmpty();
#else
    return true;
#endif
}

#endif

constexpr int kRefreshIntervalMs = 5000;
/// How often the membership state event is re-published when no delayed
/// retraction is armed, so a crashed client does not linger. Must stay well
/// under MEMBERSHIP_EXPIRY_NO_DELAYED_MS in rust/src/rtc.rs (5 min); change
/// the two together.
constexpr qint64 kMembershipRepublishIntervalMs = 60 * 1000;
constexpr qint64 kMembershipExpiryNoDelayedMs = 5 * 60 * 1000;
static_assert(kMembershipRepublishIntervalMs < kMembershipExpiryNoDelayedMs,
              "the re-publish cadence must beat the membership's expiry");
constexpr qint64 kRetractOwedForMs = kMembershipExpiryNoDelayedMs + 60 * 1000;
constexpr qint64 kMembershipExpiryLongMs = 4LL * 60 * 60 * 1000;
constexpr qint64 kRetractOwedLongForMs = kMembershipExpiryLongMs + 60 * 1000;
constexpr int kRetractAnswerTimeoutMs = 35 * 1000;
constexpr int kRetractRetryDelayMs = 2000;
constexpr int kRetractRetryMaxDelayMs = 60 * 1000;
constexpr int kUseKeyDelayMs = 5000;
constexpr int kKeySwitchFallbackMs = 15000;
constexpr int kJoinKeyGraceMs = 10000;
/// Our key indices wrap here. LiveKit rings hold 16 by default
/// (livekit-client KEY_PROVIDER_DEFAULTS) and index frames by the raw byte;
/// the bridge refuses anything above 15.
constexpr int kSendKeyIndices = 16;
/// Length of the media keys we mint: 16 raw bytes, what matrix-js-sdk
/// (RTCEncryptionManager.generateRandomKey), element-call and sable-core
/// mint. HKDF derives the same AES-128 key from any length, but receivers
/// may insist on 16: sable-core drops any other length ("not 16 bytes"),
/// which left Sable unable to hear us while Element could. We still accept
/// 16 or 32 (CallFrameCryptor::isSupportedRawKeyLength()).
constexpr int kMintedKeyBytes = 16;
constexpr int kMaxParticipants = 64;
constexpr int kMaxPendingAnnotations = kMaxParticipants;

constexpr int kReconnectFirstBackoffMs = 1000;
constexpr int kReconnectMaxBackoffMs = 8000;
constexpr int kReconnectAttemptTimeoutMs = 15000;
constexpr int kReconnectBudgetMs = 45000;
constexpr int kIceDisconnectedGraceMs = 10000;
constexpr int kMaxReconnectEpisodes = 3;
constexpr qint64 kReconnectEpisodeWindowMs = 10 * 60 * 1000;

bool containsFocusUrl(const QStringList &urls, const QString &url)
{
    return std::any_of(urls.cbegin(), urls.cend(), [&url](const QString &existing) {
        return RtcController::sameFocusUrl(existing, url);
    });
}
}

SfuCallController::SfuCallController(QObject *parent) : QObject(parent)
{
    connect(this, &SfuCallController::stateChanged, this, [this] {
        if (!active()) setStreamInfoVisible(false);
    });
    connect(this, &SfuCallController::callFailed, this,
            [this](const QString &message) { m_shownNotice = message; });
#ifdef HAVE_LIGHTNING_WEBRTC
    m_videoRouter = new SfuVideoRouter(this);
#endif
    m_participantModel = new CallParticipantModel(this);
    connect(m_participantModel, &CallParticipantModel::countChanged, this,
            &SfuCallController::participantCountChanged);
    m_shareModel = new CallShareModel(this);
    m_stageState = new CallStageState(this);
    m_stageState->setShareModel(m_shareModel);
    connect(this, &SfuCallController::mediaStateChanged, this,
            [this] { rebuildModels(); });
    m_refreshTimer.setInterval(kRefreshIntervalMs);
    connect(&m_refreshTimer, &QTimer::timeout, this,
            &SfuCallController::refreshMembership);
    m_shareAudioAppsTimer.setInterval(1500);
    connect(&m_shareAudioAppsTimer, &QTimer::timeout, this,
            &SfuCallController::refreshShareAudioApplications);
    connect(&m_refreshTimer, &QTimer::timeout, this,
            &SfuCallController::reconcileKeyLane);
    m_retractRetryTimer.setSingleShot(true);
    connect(&m_retractRetryTimer, &QTimer::timeout, this,
            &SfuCallController::retryRetraction);
    m_retractOwedForMs = kRetractOwedForMs;
    m_retractOwedLongForMs = kRetractOwedLongForMs;
    m_retractAnswerTimeoutMs = kRetractAnswerTimeoutMs;
    m_retractAnswerTimer.setSingleShot(true);
    connect(&m_retractAnswerTimer, &QTimer::timeout, this, [this] {
        if (m_retractOp == 0)
            return;
        qCWarning(lcSfuCall)
            << "no answer to a call retraction attempt; counting it as failed";
        onMembershipRetracted(m_retractOp, false, QStringLiteral("network"));
    });
    m_useKeyTimer.setSingleShot(true);
    // Precise: a coarse timer may fire up to 5% early, which comes out of the
    // time a peer has to receive the key.
    m_useKeyTimer.setTimerType(Qt::PreciseTimer);
    m_useKeyTimer.setInterval(kUseKeyDelayMs);
    connect(&m_useKeyTimer, &QTimer::timeout, this,
            &SfuCallController::adoptNewestKey);
    m_keySwitchFallbackTimer.setSingleShot(true);
    m_keySwitchFallbackTimer.setTimerType(Qt::PreciseTimer);
    m_keySwitchFallbackTimer.setInterval(kKeySwitchFallbackMs);
    connect(&m_keySwitchFallbackTimer, &QTimer::timeout, this, [this] {
        qCWarning(lcSfuCall)
            << "media key switch at the fallback,"
            << m_keySwitchFallbackTimer.interval()
            << "ms after the send index=" << m_newestKey.index
            << "answered=" << m_useKeyTimer.isActive();
        adoptNewestKey();
    });
    m_joinKeyGraceMs = kJoinKeyGraceMs;
    m_keyClock.start();

    m_reconnectFirstBackoffMs = kReconnectFirstBackoffMs;
    m_reconnectMaxBackoffMs = kReconnectMaxBackoffMs;
    m_maxReconnectEpisodes = kMaxReconnectEpisodes;
    m_reconnectEpisodeWindowMs = kReconnectEpisodeWindowMs;
    m_episodeClock.start();
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this,
            &SfuCallController::launchReconnectAttempt);
    m_reconnectAttemptTimer.setSingleShot(true);
    m_reconnectAttemptTimer.setInterval(kReconnectAttemptTimeoutMs);
    connect(&m_reconnectAttemptTimer, &QTimer::timeout, this, [this] {
        reconnectAttemptFailed(QStringLiteral("attempt_timeout"));
    });
    m_reconnectDeadlineTimer.setSingleShot(true);
    m_reconnectDeadlineTimer.setInterval(kReconnectBudgetMs);
    connect(&m_reconnectDeadlineTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Reconnecting)
            return;
        qCWarning(lcSfuCall)
            << "reconnect budget spent attempts=" << m_reconnectAttempts
            << "budgetMs=" << m_reconnectDeadlineTimer.interval();
        giveUpReconnect(connectionLostMessage());
    });
    m_iceDisconnectedTimer.setSingleShot(true);
    m_iceDisconnectedTimer.setInterval(kIceDisconnectedGraceMs);
    connect(&m_iceDisconnectedTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Connected)
            beginReconnect(QStringLiteral("ice_disconnected"));
    });
}

SfuCallController::~SfuCallController()
{
    teardown(State::Ended);
}

void SfuCallController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    teardown(State::Idle);
    abandonOutstandingMembershipWrites();
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::rtcMembershipPublished, this,
            &SfuCallController::onMembershipPublished);
    connect(m_client, &MatrixClient::rtcMembershipRetracted, this,
            &SfuCallController::onMembershipRetracted);
    connect(m_client, &MatrixClient::homeserverReachable, this,
            &SfuCallController::onHomeserverReachable);
    connect(m_client, &MatrixClient::sfuStateChanged, this,
            &SfuCallController::onSfuState);
    connect(m_client, &MatrixClient::sfuConnectionEvent, this,
            &SfuCallController::onSfuConnectionEvent);
    connect(m_client, &MatrixClient::sfuJoined, this,
            &SfuCallController::onSfuJoined);
    connect(m_client, &MatrixClient::sfuParticipantsChanged, this,
            &SfuCallController::onSfuParticipants);
    connect(m_client, &MatrixClient::sfuTrackPublished, this,
            [this](const QString &cid, const QString &sid) {
                m_publishedTrackSids.insert(cid, sid);
                const bool camera = !cid.isEmpty() && cid == m_cameraCid;
                qCInfo(lcSfuCall)
                    << "sfu published our track kind="
                    << (cid == m_audioCid ? "microphone"
                                          : camera ? "camera" : "other")
                    << "sid=" << sid;
                if (camera && cid != m_cameraNoticeCid
                    && !m_cameraNoticeCategory.isEmpty()) {
                    if (withdrawNotice(
                            userFacingError(m_cameraNoticeCategory))) {
                        qCInfo(lcSfuCall)
                            << "a camera works again; its notice withdrawn";
                    }
                    m_cameraNoticeCategory.clear();
                    m_cameraNoticeCid.clear();
                }
            });
    connect(m_client, &MatrixClient::sfuSpeakersChanged, this,
            &SfuCallController::onSfuSpeakers);
    connect(m_client, &MatrixClient::sfuConnectionQuality, this,
            &SfuCallController::onSfuConnectionQuality);
    connect(m_client, &MatrixClient::sfuRemoteDescription, this,
            &SfuCallController::onSfuRemoteDescription);
    connect(m_client, &MatrixClient::sfuRemoteCandidate, this,
            &SfuCallController::onSfuRemoteCandidate);
    connect(m_client, &MatrixClient::rtcMediaKeyReceived, this,
            &SfuCallController::onMediaKeyReceived);
    connect(m_client, &MatrixClient::rtcHandResult, this,
            &SfuCallController::onHandResult);
    connect(m_client, &MatrixClient::rtcHandChanged, this,
            &SfuCallController::onHandChanged);
    connect(m_client, &MatrixClient::rtcHandsReceived, this,
            &SfuCallController::onHandsReceived);
    connect(m_client, &MatrixClient::rtcCallReactionReceived, this,
            &SfuCallController::onCallReactionReceived);
    connect(m_client, &MatrixClient::rtcSendFinished, this,
            &SfuCallController::onRtcSendFinished);
    connect(m_client, &MatrixClient::callSignalReceived, this,
            &SfuCallController::onCallSignal);
    connect(m_client, &MatrixClient::rtcMediaKeySent, this,
            &SfuCallController::onMediaKeySent);
    connect(m_client, &MatrixClient::loggedOut, this,
            [this] { teardown(State::Ended); });
}

void SfuCallController::setRtcController(RtcController *rtc)
{
    if (m_rtc == rtc)
        return;
    if (m_rtc)
        disconnect(m_rtc, nullptr, this, nullptr);
    m_rtc = rtc;
    if (!m_rtc)
        return;
    connect(m_rtc, &RtcController::sessionChanged, this,
            [this](const QString &roomId) {
                if (!active() || roomId != m_roomId)
                    return;
                noteParticipantIdentities();
                distributeKeyIfNeeded();
                rebuildModels();
                retryPendingAnnotations();
                if (m_state == State::Connected && m_sfuGeneration != 0
                    && !m_publishFallbackTried) {
                    const QString selected = m_rtc->focusUrlFor(roomId);
                    if (!selected.isEmpty()
                        && !RtcController::sameFocusUrl(selected, m_focusUrl)) {
                        m_focusUrl = selected;
                        republishMembership();
                    }
                }
                if (m_state == State::Connected && m_sfuGeneration != 0) {
                    QStringList desired{m_focusUrl};
                    for (const QString &url : m_rtc->subscriberFocusUrlsFor(roomId)) {
                        if (!containsFocusUrl(desired, url) && desired.size() < 8)
                            desired.append(url);
                    }
                    if (desired.size() != m_connectionFocusUrls.size()
                        || !std::equal(desired.cbegin(), desired.cend(),
                                       m_connectionFocusUrls.cbegin(),
                                       RtcController::sameFocusUrl))
                        beginReconnect(QStringLiteral("focus_plan_changed"));
                }
            });
}

void SfuCallController::setMediaEngine(SfuMediaEngine *engine)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    // The assignment is inside the guard: assigning a QPointer<T> needs T
    // complete, and without WebRTC SfuMediaEngine is only forward-declared.
    m_engine = engine;
    if (!m_engine)
        return;
    m_engine->setVideoRouter(m_videoRouter);
    connect(m_engine, &SfuMediaEngine::localDescription, this,
            &SfuCallController::onEngineLocalDescription);
    connect(m_engine, &SfuMediaEngine::localCandidate, this,
            &SfuCallController::onEngineLocalCandidate);
    connect(m_engine, &SfuMediaEngine::remoteTrackAdded, this,
            [this](const QString &sid, const QString &mid, const QString &kind) {
                logRemoteSubscription(0, sid, mid, kind);
            });
    connect(m_engine, &SfuMediaEngine::failed, this,
            &SfuCallController::onEngineFailed);
    connect(m_engine, &SfuMediaEngine::transportStateChanged, this,
            &SfuCallController::onEngineTransportState);
    // Not failed(): onEngineFailed ends the call, and one broken capture
    // device must not.
    connect(m_engine, &SfuMediaEngine::publishFailed, this,
            &SfuCallController::onEnginePublishFailed);
    connect(m_engine, &SfuMediaEngine::cameraPortalReleased, this,
            &SfuCallController::onCameraPortalReleased);
    connect(m_engine, &SfuMediaEngine::cameraCaptureReady, this,
            [this](const QString &cid) {
                if (cid == m_cameraCid && m_cameraFellBack
                    && !m_cameraViaPortal && m_deviceFallbackBusyRetries > 0)
                    qCInfo(lcSfuCall) << "direct camera busy retry outcome=frames"
                                      << "retries=" << m_deviceFallbackBusyRetries;
            });
    connect(m_engine, &SfuMediaEngine::microphoneRecovered, this,
            [this](const QString &) {
                withdrawNotice(
                    userFacingError(QStringLiteral("audio_source_failed")));
            });
    connect(m_engine, &SfuMediaEngine::echoCancellationFailed, this, [this] {
        Q_EMIT callFailed(
            userFacingError(QStringLiteral("echo_cancellation_failed")));
    });
    connect(m_engine, &SfuMediaEngine::remoteMediaBlocked, this,
            [this](const QString &streamId, const QString &reason) {
                if (streamId.isEmpty())
                    return;
                const bool had = !m_blockedStreams.isEmpty();
                if (reason.isEmpty())
                    m_blockedStreams.remove(streamId);
                else
                    m_blockedStreams.insert(streamId);
                if (had != !m_blockedStreams.isEmpty())
                    Q_EMIT remoteMediaBlockedChanged();
                Q_EMIT participantsChanged();
            });
    connect(m_engine, &SfuMediaEngine::remotePlaybackFailed, this,
            &SfuCallController::onRemotePlaybackFailed);
    connect(m_engine, &SfuMediaEngine::localAudioSilent, this,
            [this](bool silent, double peakDb) {
                Q_UNUSED(peakDb);
                if (m_microphoneSilent == silent)
                    return;
                m_microphoneSilent = silent;
                Q_EMIT microphoneSilentChanged();
            });
    connect(m_engine, &SfuMediaEngine::localAudioLevel, this,
            [this](double peakDb) {
                static QElapsedTimer monotonic;
                if (!monotonic.isValid())
                    monotonic.start();
                noteMicrophoneLevelAt(peakDb, monotonic.elapsed());
            });
    connect(m_engine, &SfuMediaEngine::noiseSuppressionStatus, this,
            [this](const QString &mode, bool ok) {
                setNoiseSuppressionFailedMode(
                    ok ? QString() : mode,
                    !ok && m_engine
                        && m_engine->noiseSuppressionFellBackToWebrtc());
            });
    connect(m_engine, &SfuMediaEngine::shareAudioLevel, this,
            [this](double peakDb) {
                constexpr double kAudibleDb = -60.0;
                if (peakDb > kAudibleDb)
                    m_shareAudioLastHeard.start();
                const bool heard = m_shareAudioLastHeard.isValid()
                    && m_shareAudioLastHeard.elapsed() < 3000;
                if (heard == m_shareAudioHeard && m_shareAudioLevelKnown)
                    return;
                m_shareAudioHeard = heard;
                m_shareAudioLevelKnown = true;
                Q_EMIT shareAudioStatusChanged();
            });
    connect(m_engine, &SfuMediaEngine::shareAudioReport, this,
            [this](bool live, bool perApplication, bool excludesUs,
                   const QStringList &carried, const QStringList &failedKeys,
                   bool limitReached) {
                m_shareAudioReportLive = live;
                m_shareAudioReportPerApp = perApplication;
                m_shareAudioReportExcludesUs = excludesUs;
                m_shareAudioCarried = carried;
                m_shareAudioFailed = failedKeys;
                m_shareAudioLimitReached = limitReached;
                rebuildShareAudioApplications();
                Q_EMIT shareAudioStatusChanged();
            });
    connect(m_engine, &SfuMediaEngine::shareAudioRebuildWanted, this,
            &SfuCallController::onShareAudioRebuildWanted);
#else
    Q_UNUSED(engine);
#endif
}

void SfuCallController::onShareAudioRebuildWanted(const QString &cid)
{
    if (!active() || !m_screenSharing || cid.isEmpty()
        || cid != m_shareAudioCid)
        return;
    if (m_shareAudioRebuilds >= kMaxShareAudioRebuildsPerShare) {
        qCInfo(lcSfuCall) << "screen share audio NOT republished after the "
                             "audio daemon restarted: already done"
                          << m_shareAudioRebuilds
                          << "times in this share; recapturing in place";
        return;
    }
    ++m_shareAudioRebuilds;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull())
        return;
    qCInfo(lcSfuCall) << "screen share audio republished after the audio "
                         "daemon restarted";
    unpublishTrack(m_shareAudioCid);
    m_shareAudioCid.clear();
    resetShareAudioLevel();
    publishShareAudioTrack();
    Q_EMIT shareAudioStatusChanged();
#else
    Q_UNUSED(cid);
#endif
}

void SfuCallController::setSettings(SettingsManager *settings)
{
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = settings;
    if (!m_settings)
        return;
    restoreStoredShareAudioChoice();
    connect(m_settings, &SettingsManager::callParticipantVolumeChanged, this,
            [this](const QString &, int) { applyStoredVolumes(); });
    connect(m_settings, &SettingsManager::callShareVolumeChanged, this,
            [this](const QString &, int) { applyStoredShareVolumes(); });
    connect(m_settings, &SettingsManager::microphoneGainChanged, this,
            [this] { applyAudioState(); });
    connect(m_settings, &SettingsManager::noiseSuppressionModeChanged, this,
            [this] { applyAudioState(); });
    connect(m_settings, &SettingsManager::shareQualityChanged, this,
            [this] { applyShareQualityToLiveShare(); });
}

void SfuCallController::applyShareQualityToLiveShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || m_screenCid.isEmpty() || m_engine.isNull()
        || !m_settings)
        return;
    m_engine->setShareQuality(m_settings->effectiveShareMaxHeight(),
                              m_settings->effectiveShareFps(),
                              m_settings->shareBitrateCapKbps());
    if (!m_engine->applyShareQualityLive(m_screenCid)) {
        qCWarning(lcSfuCall)
            << "screen share quality changed but the live share could not be "
               "retargeted";
    }
#endif
}

void SfuCallController::restoreStoredShareAudioChoice()
{
    if (!m_settings)
        return;
    const int mode = m_settings->callShareAudioMode();
    const QStringList apps = m_settings->callShareAudioApps();
    if (m_screenSharing)
        return;
    m_shareAudioMode = mode;
    m_shareAudioLastOnMode = mode != 0 ? mode : 1;
    m_shareAudioApps = mode == 2 ? apps : QStringList();
    Q_EMIT mediaStateChanged();
    Q_EMIT shareAudioStatusChanged();
    rebuildShareAudioApplications();
}

void SfuCallController::persistShareAudioChoice()
{
    if (!m_settings)
        return;
    m_settings->setCallShareAudioMode(m_shareAudioMode);
    if (m_shareAudioMode == 2)
        m_settings->setCallShareAudioApps(m_shareAudioApps);
}

void SfuCallController::setScreenCastPortal(ScreenCastPortal *portal)
{
    if (m_portal == portal)
        return;
    if (m_portal)
        disconnect(m_portal, nullptr, this, nullptr);
    m_portal = portal;
    if (!m_portal)
        return;
    connect(m_portal, &ScreenCastPortal::ready, this,
            [this](unsigned nodeId, int pipewireFd) {
                qCInfo(lcSfuCall) << "screen share portal ready node="
                                  << nodeId << "remote_fd="
                                  << (pipewireFd >= 0);
                const bool reconnecting = m_state == State::Reconnecting;
                if (!active() || reconnecting
                    || !startScreenShare(static_cast<int>(nodeId),
                                         pipewireFd)) {
                    qCWarning(lcSfuCall)
                        << "screen share refused after portal grant active="
                        << active() << "reconnecting=" << reconnecting;
                    closePortalFd(pipewireFd);
                    if (m_portal)
                        m_portal->cancel();
                    if (reconnecting)
                        Q_EMIT callFailed(shareWhileReconnectingMessage());
                }
            });
    connect(m_portal, &ScreenCastPortal::cancelled, this, [] {
    });
    connect(m_portal, &ScreenCastPortal::failed, this,
            [this](const QString &category) {
                qCWarning(lcSfuCall) << "screen share portal failed category="
                                     << category;
#if defined(HAVE_LIGHTNING_WEBRTC) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
                if (active()
                    && portalFailureFallsBackToDisplays(
                        category, QGuiApplication::platformName(),
                        qEnvironmentVariable("XDG_SESSION_TYPE"),
                        qEnvironmentVariable("WAYLAND_DISPLAY"),
                        qEnvironmentVariable("DISPLAY"),
                        SfuMediaEngine::elementAvailable(
                            SfuMediaEngine::x11ScreenCaptureElementName()))) {
                    qCInfo(lcSfuCall)
                        << "screen share route=fallback-displays after portal "
                           "refusal category=" << category
                        << "(X11 session; Lightning's own display picker, no "
                           "auto-select)";
                    offerLinuxDisplayPicker(false);
                    return;
                }
#endif
                Q_EMIT callFailed(category == QLatin1String("no_portal")
                                      ? tr("Screen sharing isn't available on "
                                           "this desktop.")
                                      : tr("Screen sharing couldn't start."));
            });
}

void SfuCallController::setCameraPortal(CameraPortal *portal)
{
    if (m_cameraPortal == portal)
        return;
    if (m_cameraPortal)
        disconnect(m_cameraPortal, nullptr, this, nullptr);
    m_cameraPortal = portal;
    if (!m_cameraPortal)
        return;
    connect(m_cameraPortal, &CameraPortal::ready, this, [this](int pipewireFd) {
        qCInfo(lcSfuCall) << "camera portal ready remote_fd="
                          << (pipewireFd >= 0);
        if (!m_cameraAwaitingPortal) {
            qCInfo(lcSfuCall) << "camera portal grant arrived for a camera "
                                 "that is no longer wanted; releasing";
            closePortalFd(pipewireFd);
            return;
        }
        m_cameraAwaitingPortal = false;
        if (!active() || !cameraEngineReady() || !m_client) {
            closePortalFd(pipewireFd);
            abandonPendingCamera();
            return;
        }
        publishCameraTrack(pipewireFd);
        applyVideoState();
        Q_EMIT mediaStateChanged();
    });
    connect(m_cameraPortal, &CameraPortal::cancelled, this, [this] {
        qCInfo(lcSfuCall) << "camera portal declined";
        abandonPendingCamera();
    });
    connect(m_cameraPortal, &CameraPortal::failed, this,
            [this](const QString &category) {
                qCWarning(lcSfuCall) << "camera portal failed category="
                                     << category;
                if (m_cameraAwaitingPortal && active()
                    && cameraFailureStep(category,true,
                                         cameraDeviceNodeVisible(),
                                         m_cameraFellBack
                                             || m_cameraFellBackToPortal)
                        == CameraFailureStep::OpenDeviceNode) {
                    m_cameraAwaitingPortal = false;
                    fallBackToDeviceCamera(category);
                    return;
                }
                abandonPendingCamera();
                announceCameraNotice(
                    cameraNoticeCategory(category,true,
                                         m_cameraPortalReportedCamera),
                    QString());
            });
}

SfuCallController::LinuxCameraRoute SfuCallController::linuxCameraRoute(
    bool sandboxed, bool portalUsable, bool directDeviceVisible,
    bool cameraChosen)
{
    if (cameraChosen)
        return LinuxCameraRoute::Direct;
    if (sandboxed && directDeviceVisible && !portalUsable)
        return LinuxCameraRoute::Direct;
    if (sandboxed)
        return LinuxCameraRoute::Portal;
    if (portalUsable && !cameraChosen)
        return LinuxCameraRoute::Portal;
    if (directDeviceVisible)
        return LinuxCameraRoute::Direct;
    if (portalUsable)
        return LinuxCameraRoute::Portal;
    return LinuxCameraRoute::Direct;
}

bool SfuCallController::directCameraFallsBackToPortal(const QString &category,
                                                      bool viaPortal,
                                                      bool portalWired,
                                                      bool alreadyFellBack)
{
    return !viaPortal && portalWired && !alreadyFellBack
        && (category == QLatin1String("camera_busy")
            || category == QLatin1String("camera_resource"));
}

SfuCallController::CameraFailureStep SfuCallController::cameraFailureStep(
    const QString &category, bool viaPortal, bool directDeviceVisible,
    bool alreadyFellBack)
{
    if (!viaPortal || !directDeviceVisible || alreadyFellBack)
        return CameraFailureStep::Notice;
    if (category == QLatin1String("camera_no_frames")
        || category == QLatin1String("camera_failed")
        || category == QLatin1String("no_portal")
        || category == QLatin1String("no_pipewire_remote"))
        return CameraFailureStep::OpenDeviceNode;
    return CameraFailureStep::Notice;
}

QString SfuCallController::cameraNoticeCategory(const QString &category,
                                                bool viaPortal,
                                                bool portalReportedCamera)
{
    if (category == QLatin1String("no_portal"))
        return QStringLiteral("camera_portal_unavailable");
    if (category == QLatin1String("not_allowed"))
        return QStringLiteral("camera_not_allowed");
    if (category == QLatin1String("no_pipewire_remote")
        || category == QLatin1String("portal_failed")
        || category == QLatin1String("timeout")
        || category == QLatin1String("busy"))
        return QStringLiteral("camera_failed");
    if (category == QLatin1String("camera_resource"))
        return QStringLiteral("camera_busy");
    if (viaPortal
        && (category == QLatin1String("camera_no_frames")
            || category == QLatin1String("camera_failed"))) {
        return portalReportedCamera
            ? QStringLiteral("camera_portal_no_picture")
            : QStringLiteral("camera_portal_no_camera");
    }
    return category;
}

QString SfuCallController::cameraPortalNotice(const QString &category,
                                              bool flatpak)
{
    if (category == QLatin1String("camera_portal_no_camera")) {
        return flatpak
            ? tr("Your desktop's camera portal offered Lightning no camera. "
                 "If the camera works in other apps, allow Lightning to "
                 "use all devices (in Flatseal: \"All devices\"), then turn "
                 "the camera on again.")
            : tr("Your desktop's camera portal offered Lightning no "
                 "camera.");
    }
    if (category == QLatin1String("camera_portal_no_picture")) {
        return flatpak
            ? tr("Your desktop's camera portal sent no picture. To let "
                 "Lightning open the camera itself, allow it to use all "
                 "devices (in Flatseal: \"All devices\"), then turn the "
                 "camera on again.")
            : tr("Your desktop's camera portal sent no picture from the "
                 "camera.");
    }
    if (category == QLatin1String("camera_portal_unavailable")) {
        return flatpak
            ? tr("This desktop has no camera portal. Install "
                 "xdg-desktop-portal, or allow Lightning to use all devices "
                 "(in Flatseal: \"All devices\").")
            : tr("This desktop has no camera portal, so the camera can't be "
                 "used.");
    }
    return {};
}

bool SfuCallController::cameraDeviceNodeVisible() const
{
    if (m_deviceNodeVisibleForTest >= 0)
        return m_deviceNodeVisibleForTest == 1;
#ifdef HAVE_LIGHTNING_WEBRTC
    return v4l2DeviceNodeVisible();
#else
    return false;
#endif
}

bool SfuCallController::cameraEngineReady() const
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull())
        return true;
#endif
    return static_cast<bool>(m_cameraPublisherForTest);
}

bool SfuCallController::cameraSandboxed() const
{
    if (m_cameraSandboxedForTest >= 0)
        return m_cameraSandboxedForTest == 1;
#ifdef HAVE_LIGHTNING_WEBRTC
    return runningSandboxed();
#else
    return false;
#endif
}

bool SfuCallController::cameraPortalUsable() const
{
    if (m_cameraPortal.isNull())
        return false;
    if (m_cameraPortalUsableForTest >= 0)
        return m_cameraPortalUsableForTest == 1;
    return CameraPortal::available() && CameraPortal::cameraPresent();
}

void SfuCallController::requestCameraAccess()
{
    if (m_cameraRequestForTest) {
        m_cameraRequestForTest();
        return;
    }
    if (m_cameraPortal)
        m_cameraPortal->requestAccess();
}

void SfuCallController::fallBackToDeviceCamera(const QString &category,
                                               const QString &retiredCid)
{
    m_cameraFellBack = true;
    m_deviceFallbackGeneration = m_generation;
    ++m_deviceFallbackToken;
    m_deviceFallbackBusyRetries = 0;
    m_deviceFallbackDeadlineMs = 0;
    const bool alreadyReleased =
        !retiredCid.isEmpty() && retiredCid == m_releasedPortalCid;
    m_waitingPortalReleaseCid = alreadyReleased ? QString() : retiredCid;
    qCWarning(lcSfuCall) << "camera portal gave no picture; waiting for "
                            "capture teardown before direct device category="
                         << category << "retiredCid=" << retiredCid
                         << "alreadyReleased=" << alreadyReleased;
    m_cameraOn = false;
    applyVideoState();
    m_cameraOn = true;
    m_cameraViaPortal = false;
    if (m_waitingPortalReleaseCid.isEmpty()) {
        publishDeviceFallback(m_deviceFallbackToken);
    } else {
        const quint64 token = m_deviceFallbackToken;
        const quint64 generation = m_generation;
        const QString waitingFor = m_waitingPortalReleaseCid;
        QTimer::singleShot(m_deviceFallbackWindowMs, this,
                           [this, token, generation, waitingFor] {
            if (token != m_deviceFallbackToken || generation != m_generation
                || m_waitingPortalReleaseCid != waitingFor)
                return;
            qCWarning(lcSfuCall) << "portal camera release not reported "
                                    "within the fallback window; opening the "
                                    "device anyway";
            m_waitingPortalReleaseCid.clear();
            publishDeviceFallback(token);
        });
    }
    Q_EMIT mediaStateChanged();
}

void SfuCallController::onCameraPortalReleased(const QString &cid)
{
    m_releasedPortalCid = cid;
    if (cid != m_waitingPortalReleaseCid
        || m_deviceFallbackGeneration != m_generation)
        return;
    m_waitingPortalReleaseCid.clear();
    qCInfo(lcSfuCall) << "portal camera remote closed; direct fallback may start";
    publishDeviceFallback(m_deviceFallbackToken);
}

void SfuCallController::publishDeviceFallback(quint64 token)
{
    if (token != m_deviceFallbackToken
        || m_deviceFallbackGeneration != m_generation || !active() || !m_cameraOn
        || !m_cameraFellBack || !m_waitingPortalReleaseCid.isEmpty()
        || !cameraEngineReady())
        return;
    if (m_deviceFallbackDeadlineMs == 0)
        m_deviceFallbackDeadlineMs = QDateTime::currentMSecsSinceEpoch()
            + m_deviceFallbackWindowMs;
    qCInfo(lcSfuCall) << "direct camera fallback attempt="
                      << (m_deviceFallbackBusyRetries + 1);
    publishCameraTrack(-1);
}

void SfuCallController::fallBackToPortalCamera(const QString &category)
{
    m_cameraFellBackToPortal = true;
    qCWarning(lcSfuCall) << "camera device could not be opened; trying the "
                            "camera portal, which shares the device "
                            "category=" << category;
    m_cameraOn = false;
    applyVideoState();
    m_cameraOn = true;
    m_cameraViaPortal = true;
    m_cameraPortalReportedCamera = cameraPortalUsable();
    m_cameraAwaitingPortal = true;
    Q_EMIT mediaStateChanged();
    requestCameraAccess();
}

bool SfuCallController::cameraChosen() const
{
    if (m_cameraChosenForTest >= 0)
        return m_cameraChosenForTest == 1;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull()) {
        const SfuMediaEngine::DeviceChoice choice = m_engine->cameraChoice();
        return !choice.id.isEmpty() || choice.preferredMissing;
    }
#endif
    return false;
}

SfuCallController::LinuxShareRoute SfuCallController::linuxShareRoute(
    bool portalAvailable, const QString &platformName,
    const QString &sessionType, const QString &waylandDisplay,
    const QString &x11Display, bool captureElementPresent)
{
    if (portalAvailable)
        return LinuxShareRoute::Portal;

    // Wayland is refused before any X11 clause: there is no way to capture a
    // Wayland desktop without the portal, and XWayland provides a DISPLAY
    // whose root window is black, so an X11 pipeline would send a black
    // rectangle. Any of the three signals is enough.
    if (platformName.startsWith(QLatin1String("wayland"), Qt::CaseInsensitive)
        || sessionType.compare(QLatin1String("wayland"), Qt::CaseInsensitive)
            == 0
        || !waylandDisplay.isEmpty()) {
        return LinuxShareRoute::RefuseWaylandNeedsPortal;
    }

    if (x11Display.isEmpty())
        return LinuxShareRoute::RefuseNoDisplayServer;
    if (!captureElementPresent)
        return LinuxShareRoute::RefuseNoCaptureElement;
    return LinuxShareRoute::FallbackDisplays;
}

bool SfuCallController::portalFailureFallsBackToDisplays(
    const QString &category, const QString &platformName,
    const QString &sessionType, const QString &waylandDisplay,
    const QString &x11Display, bool captureElementPresent)
{
    static const QStringList kFallbackCategories{
        QStringLiteral("x11_unsupported"), QStringLiteral("session_refused"),
        QStringLiteral("no_answer"), QStringLiteral("timeout")};
    if (!kFallbackCategories.contains(category))
        return false;
    if (platformName.startsWith(QLatin1String("wayland"), Qt::CaseInsensitive)
        || sessionType.compare(QLatin1String("wayland"), Qt::CaseInsensitive)
            == 0
        || !waylandDisplay.isEmpty())
        return false;
    if (!platformName.startsWith(QLatin1String("xcb"), Qt::CaseInsensitive))
        return false;
    return !x11Display.isEmpty() && captureElementPresent;
}

QString SfuCallController::linuxShareRefusal(LinuxShareRoute route,
                                             bool sandboxed)
{
    switch (route) {
    case LinuxShareRoute::Portal:
    case LinuxShareRoute::FallbackDisplays:
        return {};
    case LinuxShareRoute::RefuseWaylandNeedsPortal:
        return tr("Screen sharing on Wayland needs xdg-desktop-portal, and "
                  "it isn't responding. Install or start the portal for your "
                  "desktop — for example xdg-desktop-portal-kde or "
                  "xdg-desktop-portal-gnome — then try again.");
    case LinuxShareRoute::RefuseNoCaptureElement:
        if (sandboxed) {
            return tr("Screen sharing isn't available in this sandboxed "
                      "(Flatpak or Snap) build on an X11 session: it can "
                      "only share through the desktop's screen-sharing "
                      "portal, and none is available, and GStreamer plugins "
                      "installed on your system cannot be used from the "
                      "sandbox. To share your screen, use the AppImage or a "
                      "distribution package of Lightning, which capture an "
                      "X11 screen directly, or log into a Wayland session, "
                      "where your desktop's portal provides screen sharing. "
                      "If your desktop's xdg-desktop-portal supports screen "
                      "casting on X11, make sure it is installed and "
                      "running.");
        }
        return tr("Screen sharing needs GStreamer's %1 element, which isn't "
                  "installed. Install the gst-plugins-good package and try "
                  "again.")
            .arg(QLatin1String(
                SfuMediaEngine::x11ScreenCaptureElementName()));
    case LinuxShareRoute::RefuseNoDisplayServer:
        return tr("Screen sharing isn't available: no display server was "
                  "found.");
    }
    return tr("Screen sharing isn't available on this desktop.");
}

QRect SfuCallController::validX11CaptureRect(const QRect &nativeGeometry)
{
    // No arithmetic: the rectangle must already be native and root-relative
    // (see nativeScreenRect()). Only refuses shapes ximagesrc cannot take;
    // its coordinates are unsigned, so a negative origin would wrap.
    if (nativeGeometry.width() <= 0 || nativeGeometry.height() <= 0)
        return {};
    if (nativeGeometry.x() < 0 || nativeGeometry.y() < 0)
        return {};
    return nativeGeometry;
}

QRect SfuCallController::nativeScreenRect(const QScreen *screen)
{
    if (!screen)
        return {};
#if defined(Q_OS_LINUX) && defined(LIGHTNING_HAVE_QPA_SCREEN)
    const QPlatformScreen *platform = screen->handle();
    if (!platform)
        return {};
    return validX11CaptureRect(platform->geometry());
#else
    Q_UNUSED(screen);
    return {};
#endif
}

QRect SfuCallController::physicalRectForScreenNamed(const QString &name)
{
    if (name.isEmpty())
        return {};
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        if (screen && screen->name() == name)
            return nativeScreenRect(screen);
    }
    return {};
}

void SfuCallController::offerLinuxDisplayPicker(bool autoSelectSingle)
{
#if defined(HAVE_LIGHTNING_WEBRTC) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    if (m_state == State::Reconnecting) {
        Q_EMIT callFailed(shareWhileReconnectingMessage());
        return;
    }
    if (!populateLinuxDisplaySources()) {
        Q_EMIT callFailed(tr("No display is available to share."));
        return;
    }
    Q_EMIT screenShareSourcesChanged();
    if (autoSelectSingle && m_screenShareSources.size() == 1) {
        chooseScreenShareSource(0);
        return;
    }
    Q_EMIT screenShareSourcesAvailable();
#else
    Q_UNUSED(autoSelectSingle);
#endif
}

bool SfuCallController::populateLinuxDisplaySources()
{
    m_screenShareSources.clear();
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QWindow *ownWindow = QGuiApplication::focusWindow();
    const QScreen *ownScreen = ownWindow ? ownWindow->screen() : nullptr;
    if (!ownScreen)
        ownScreen = QGuiApplication::primaryScreen();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        if (!screen)
            continue;
        const QRect rect = nativeScreenRect(screen);
        if (!rect.isValid())
            continue;
        m_screenShareSources.append(QVariantMap{
            { QStringLiteral("index"), i },
            { QStringLiteral("name"), screen->name() },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"), QStringLiteral("%1 x %2")
                                              .arg(rect.width())
                                              .arg(rect.height()) },
            { QStringLiteral("primary"),
              screen == QGuiApplication::primaryScreen() },
            { QStringLiteral("current"), screen == ownScreen },
        });
    }
    return !m_screenShareSources.isEmpty();
}

void SfuCallController::requestScreenShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || m_engine.isNull())
        return;
    if (m_state == State::Reconnecting) {
        Q_EMIT callFailed(shareWhileReconnectingMessage());
        return;
    }
    const bool portalUsable =
        !m_portal.isNull() && ScreenCastPortal::available();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    if (!portalUsable) {
        qCWarning(lcSfuCall) << "screen share unavailable portal="
                             << !m_portal.isNull();
        Q_EMIT callFailed(
            tr("Screen sharing isn't available on this desktop."));
        return;
    }
#endif
    if (m_screenSharing)
        stopScreenShare();
    if (!m_portal.isNull() && m_portal->busy()) {
        qCInfo(lcSfuCall) << "screen share already being chosen; ignoring";
        return;
    }
    qCInfo(lcSfuCall) << "screen share requested";
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_screenShareSources.clear();
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QWindow *ownWindow = QGuiApplication::focusWindow();
    const QScreen *ownScreen = ownWindow ? ownWindow->screen() : nullptr;
    if (!ownScreen)
        ownScreen = QGuiApplication::primaryScreen();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        QSize g = screen->geometry().size() * screen->devicePixelRatio();
        int captureIndex = i;
        int pixelWidth = 0;
        int pixelHeight = 0;
        if (lightning::wincap::displayForDeviceName(screen->name(),
                                                    &captureIndex,
                                                    &pixelWidth,
                                                    &pixelHeight)
            && pixelWidth > 0 && pixelHeight > 0) {
            g = QSize(pixelWidth, pixelHeight);
        }
        m_screenShareSources.append(QVariantMap{
            { QStringLiteral("index"), captureIndex },
            { QStringLiteral("name"), screen->name() },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"),
              QStringLiteral("%1 x %2").arg(g.width()).arg(g.height()) },
            { QStringLiteral("primary"),
              screen == QGuiApplication::primaryScreen() },
            { QStringLiteral("current"), screen == ownScreen },
        });
    }
    const QList<lightning::wincap::WindowInfo> windows =
        lightning::wincap::enumerateWindows();
    const QSet<qint64> ourAncestors = windows.isEmpty()
        ? QSet<qint64>()
        : lightning::shareaudio::ancestorsOf(
              QCoreApplication::applicationPid(),
              lightning::shareaudio::processParents());
    for (const lightning::wincap::WindowInfo &window : windows) {
        const bool containsUs =
            ourAncestors.contains(static_cast<qint64>(window.pid));
        m_screenShareSources.append(QVariantMap{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), window.handle },
            { QStringLiteral("name"), window.title },
            { QStringLiteral("application"), window.application },
            { QStringLiteral("geometry"),
              QStringLiteral("%1 x %2").arg(window.width).arg(window.height) },
            { QStringLiteral("primary"), false },
            { QStringLiteral("current"), false },
            { QStringLiteral("audioAppKey"),
              containsUs ? QString() : window.executable },
            { QStringLiteral("audioAppContainsUs"), containsUs },
        });
    }

    if (m_screenShareSources.isEmpty()) {
        Q_EMIT callFailed(tr("No display is available to share."));
        return;
    }
    Q_EMIT screenShareSourcesChanged();
    if (m_screenShareSources.size() == 1) {
        chooseScreenShareSource(0);
        return;
    }
    Q_EMIT screenShareSourcesAvailable();
#else
    const LinuxShareRoute route = linuxShareRoute(
        portalUsable, QGuiApplication::platformName(),
        qEnvironmentVariable("XDG_SESSION_TYPE"),
        qEnvironmentVariable("WAYLAND_DISPLAY"),
        qEnvironmentVariable("DISPLAY"),
        SfuMediaEngine::elementAvailable(
            SfuMediaEngine::x11ScreenCaptureElementName()));
    qCInfo(lcSfuCall) << "screen share route=" << static_cast<int>(route);
    switch (route) {
    case LinuxShareRoute::Portal: {
        const bool x11FallbackPossible = portalFailureFallsBackToDisplays(
            QStringLiteral("no_answer"), QGuiApplication::platformName(),
            qEnvironmentVariable("XDG_SESSION_TYPE"),
            qEnvironmentVariable("WAYLAND_DISPLAY"),
            qEnvironmentVariable("DISPLAY"),
            SfuMediaEngine::elementAvailable(
                SfuMediaEngine::x11ScreenCaptureElementName()));
        constexpr int kX11PortalPreparationBoundMs = 8000;
        constexpr int kX11PortalPickerBoundMs = 30000;
        if (x11FallbackPossible)
            qCInfo(lcSfuCall) << "screen share portal bounded (X11 session) "
                                 "preparation-ms="
                              << kX11PortalPreparationBoundMs
                              << "picker-ms=" << kX11PortalPickerBoundMs;
        m_portal->requestShare(
            ScreenCastPortal::Monitor | ScreenCastPortal::Window,
            x11FallbackPossible ? kX11PortalPreparationBoundMs : 0,
            x11FallbackPossible ? kX11PortalPickerBoundMs : 0);
        return;
    }
    case LinuxShareRoute::FallbackDisplays:
        offerLinuxDisplayPicker(true);
        return;
    case LinuxShareRoute::RefuseWaylandNeedsPortal:
    case LinuxShareRoute::RefuseNoCaptureElement:
    case LinuxShareRoute::RefuseNoDisplayServer:
        Q_EMIT callFailed(linuxShareRefusal(route, runningSandboxed()));
        return;
    }
#endif
#endif
}

void SfuCallController::chooseScreenShareSource(int index)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_screenShareSources.isEmpty())
        return;
    if (m_state == State::Reconnecting) {
        m_screenShareSources.clear();
        Q_EMIT screenShareSourcesChanged();
        Q_EMIT callFailed(shareWhileReconnectingMessage());
        return;
    }
    if (index < 0 || index >= m_screenShareSources.size()) {
        qCWarning(lcSfuCall) << "screen share source out of range";
        cancelScreenShareSelection();
        return;
    }
    const QVariantMap chosen = m_screenShareSources.at(index).toMap();
    const quint64 windowHandle =
        chosen.value(QStringLiteral("windowHandle")).toULongLong();
    const int displayIndex = chosen.value(QStringLiteral("index")).toInt();
    const bool isWindow = windowHandle != 0;

    QRect captureRect;
    bool displayGone = false;
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    if (!isWindow) {
        captureRect =
            physicalRectForScreenNamed(chosen.value(QStringLiteral("name"))
                                           .toString());
        displayGone = !captureRect.isValid();
    }
#endif

    m_screenShareSources.clear();
    Q_EMIT screenShareSourcesChanged();
    if (displayGone) {
        qCWarning(lcSfuCall) << "chosen display is no longer connected";
        Q_EMIT callFailed(tr("That display isn't connected any more."));
        return;
    }
    if (!startScreenShare(isWindow ? -1 : displayIndex, -1, windowHandle,
                          captureRect)) {
        Q_EMIT callFailed(isWindow
                              ? tr("Couldn't start sharing that window.")
                              : tr("Couldn't start sharing that display."));
    }
#else
    Q_UNUSED(index);
#endif
}

void SfuCallController::cancelScreenShareSelection()
{
    if (m_screenShareSources.isEmpty())
        return;
    m_screenShareSources.clear();
    Q_EMIT screenShareSourcesChanged();
}

bool SfuCallController::active() const
{
    return m_state != State::Idle && m_state != State::Ended
        && m_state != State::Failed;
}

bool SfuCallController::sfuSessionLive() const
{
    return active()
        && (m_state != State::Reconnecting
            || m_reconnectPhase == ReconnectPhase::Joined);
}

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
void SfuCallController::startDemoCall(const QString &roomId,
                                      bool withScreenShare)
{
    if (roomId.isEmpty())
        return;
    m_roomId = roomId;
    m_demoCall = true;

    struct DemoPerson {
        const char *id;
        const char *name;
        const char *avatar;
        bool local;
        bool micMuted;
        bool hand;
    };
    static const DemoPerson kPeople[] = {
        { "@alex:lightning.example",   "Alex Morgan", "mxc://lightning.example/avatar-alex",   true,  false, false },
        { "@maya:lightning.example",   "Maya Chen",   "mxc://lightning.example/avatar-maya",   false, false, false },
        { "@jordan:lightning.example", "Jordan Lee",  "mxc://lightning.example/avatar-jordan", false, true,  true  },
        { "@sam:lightning.example",    "Sam Rivera",  "mxc://lightning.example/avatar-sam",    false, false, false },
        { "@aisha:lightning.example",  "Aisha Khan",  "mxc://lightning.example/avatar-aisha",  false, true,  false },
        { "@priya:lightning.example",  "Priya Shah",  "mxc://lightning.example/avatar-priya",  false, false, false },
    };

    QVector<CallParticipantRow> rows;
    rows.reserve(int(std::size(kPeople)));
    for (const DemoPerson &p : kPeople) {
        CallParticipantRow row;
        row.identity = QString::fromLatin1(p.id);
        row.sid = QStringLiteral("PA_demo_") + QString::fromLatin1(p.name)
                      .remove(QLatin1Char(' '));
        row.userId = QString::fromLatin1(p.id);
        row.displayName = QString::fromLatin1(p.name);
        row.avatarMxc = QString::fromLatin1(p.avatar);
        row.local = p.local;
        row.micKnown = true;
        row.micMuted = p.micMuted;
        row.cameraKnown = true;
        row.cameraOn = false;
        rows.append(row);
    }
    if (withScreenShare) {
        rows.resize(4);
        rows[1].screenSharing = true;
        rows[1].screenTrackKey = QStringLiteral("TR_demo_screen");
    }
    if (m_participantModel) {
        m_participantModel->applyParticipants(rows);
        for (const DemoPerson &p : kPeople) {
            if (p.hand)
                m_participantModel->setHandRaised(QString::fromLatin1(p.id),
                                                  true);
        }
        QHash<QString, bool> active;
        QHash<QString, qreal> level;
        active.insert(rows[1].sid, true);
        level.insert(rows[1].sid, 0.62);
        m_participantModel->applySpeakers(active, level);
    }
    rebuildShareModel();

    m_micMuted = false;
    m_cameraOn = false;
    m_screenSharing = false;
    setState(State::Connected);
    Q_EMIT mediaStateChanged();
    Q_EMIT participantsChanged();
}

void SfuCallController::endDemoCall()
{
    if (!m_demoCall && m_state == State::Idle)
        return;
    m_demoCall = false;
    if (m_participantModel)
        m_participantModel->applyParticipants({});
    rebuildShareModel();
    m_screenSharing = false;
    m_cameraOn = false;
    m_roomId.clear();
    setState(State::Idle);
    Q_EMIT mediaStateChanged();
    Q_EMIT participantsChanged();
}
#endif

void SfuCallController::setState(State state, const QString &error)
{
    if (m_state == state && m_lastError == error)
        return;
    const bool hadOutstandingFailure = m_failureAnnounced;
    m_state = state;
    m_lastError = error;
    if (!error.isEmpty()) {
        m_failureAnnounced = true;
        m_announcedFailure = error;
    }
    Q_EMIT stateChanged();

    const bool pastTheJoinGate = state == State::Authorizing
        || state == State::Connecting || state == State::Connected
        || state == State::Reconnecting;
    if (hadOutstandingFailure && error.isEmpty() && pastTheJoinGate) {
        m_failureAnnounced = false;
        qCInfo(lcSfuCall) << "the previous call failure no longer applies; "
                             "withdrawing it";
        withdrawNotice(m_announcedFailure);
        return;
    }
    if (state == State::Idle)
        m_failureAnnounced = false;
}

QString SfuCallController::userFacingError(const QString &category) const
{
    if (category == QLatin1String("membership_forbidden"))
        return tr("You don't have permission to join calls in this room. "
                  "A room admin can raise your power level in it; Lightning "
                  "can't change what call membership itself requires.");
    if (category == QLatin1String("forbidden")
        || category == QLatin1String("sfu_forbidden")
        || category == QLatin1String("publish_denied"))
        return tr("The calling service refused to connect you to this call.");
    if (category == QLatin1String("unsupported")
        || category == QLatin1String("sfu_not_found"))
        return tr("The calling service this call uses didn't answer. It's "
                  "chosen by whoever started the call, not by your "
                  "homeserver.");
    if (category == QLatin1String("unrecognized")
        || category == QLatin1String("not_found"))
        return tr("Your homeserver doesn't support Matrix calls.");
    if (category == QLatin1String("rate_limited"))
        return tr("Too many attempts. Try again in a moment.");
    if (category == QLatin1String("share_audio_failed"))
        return tr("Your screen is being shared without its sound — the "
                  "audio capture couldn't be started.");
    if (category == QLatin1String("share_audio_unavailable"))
        return tr("Your screen is being shared without its sound — this "
                  "system has no way to capture what it is playing.");
    if (category == QLatin1String("share_audio_apps_unavailable"))
        return tr("Your screen is being shared without its sound — the "
                  "apps you chose can't be captured on this system.");
    if (category == QLatin1String("focus_unroutable"))
        return tr("This call's service is on a private network address, "
                  "which Lightning won't connect to. Whoever set up the call "
                  "needs to give it an address reachable from the internet.");

    if (category == QLatin1String("focus_unresolved")
        || category == QLatin1String("focus_resolve_timeout"))
        return tr("Lightning couldn't look up this call's service. Its name "
                  "doesn't resolve, or this network's DNS isn't answering.");
    if (category == QLatin1String("focus_private_name"))
        return tr("This call's service is advertised under a name that only "
                  "means \"this computer\", so Lightning won't connect to "
                  "it. Whoever set up the call needs to give it a real "
                  "address.");
    if (category == QLatin1String("sfu_unreachable"))
        return tr("This network has no route to the call's service. If "
                  "you're on a VPN or a restricted network, that's the "
                  "first thing to check.");
    if (category == QLatin1String("sfu_refused_connection"))
        return tr("The call's service refused the connection — nothing is "
                  "listening at the address it published.");
    if (category == QLatin1String("connect_blocked"))
        return tr("Something on this computer blocked the connection to the "
                  "call's service — a firewall or a security policy.");
    if (category == QLatin1String("connect_timeout"))
        return tr("The call's service didn't answer in time.");
    if (category == QLatin1String("tls_failed"))
        return tr("Lightning couldn't open a secure connection to the "
                  "call's service. Its certificate or its TLS setup was "
                  "refused.");
    if (category == QLatin1String("ws_rejected")
        || category == QLatin1String("ws_handshake_failed"))
        return tr("The call's service answered, but not as a call service. "
                  "Something between you and it — a proxy or a sign-in "
                  "portal — may be intercepting the connection.");
    if (category == QLatin1String("network")
        || category == QLatin1String("connect_failed")
        || category == QLatin1String("connection_lost")
        || category == QLatin1String("transport_failed")
        || category == QLatin1String("send_failed"))
        return tr("Couldn't connect to the call.");
    if (category == QLatin1String("server_error"))
        return tr("The calling service is having trouble.");
    if (category == QLatin1String("invalid")
        || category == QLatin1String("invalid_transport")
        || category == QLatin1String("invalid_request")
        || category == QLatin1String("focus_url_invalid")
        || category == QLatin1String("ws_frame_too_large")
        || category == QLatin1String("unknown"))
        return tr("This call's service isn't set up correctly, so Lightning "
                  "couldn't connect to it.");
    if (category == QLatin1String("screen_share_source_closed"))
        return tr("The window you were sharing was closed.");
    if (category.startsWith(QLatin1String("screen_share")))
        return tr("Screen sharing couldn't start.");
    if (category == QLatin1String("camera_source_closed"))
        return tr("Your camera stopped.");
    if ((category == QLatin1String("camera_failed")
         || category == QLatin1String("camera_no_frames"))
        && sandboxenv::isSnap()) {
        return tr("Your camera isn't available. Lightning is a snap: run "
                  "snap connect lightning:camera");
    }
    if (category == QLatin1String("camera_failed"))
        return tr("Your camera isn't available.");
    if (category == QLatin1String("camera_busy"))
        return tr("Lightning couldn't open the camera. It may be in use by "
                  "another app, or unplugged.");
    if (category == QLatin1String("camera_no_frames"))
        return tr("The camera sent no picture. If it's a virtual camera, "
                  "make sure something is feeding it.");
    if (category == QLatin1String("camera_not_allowed"))
        return tr("Camera access is turned off in your system settings.");
    if (category.startsWith(QLatin1String("camera_portal_"))) {
        const QString notice =
            cameraPortalNotice(category, sandboxenv::isFlatpak());
        if (!notice.isEmpty())
            return notice;
    }
    if (category == QLatin1String("camera_unavailable"))
        return tr("The camera you chose isn't available, so no camera was "
                  "turned on. Choose a camera in Settings.");
    if (category == QLatin1String("camera_list_unavailable"))
        return tr("Lightning couldn't read the list of cameras, so no camera "
                  "was turned on. Restart Lightning, or choose the system "
                  "default camera in Settings.");
    if (category == QLatin1String("audio_source_failed"))
        return tr("Your microphone isn't available.");
    if (category == QLatin1String("echo_cancellation_failed"))
        return tr("Echo cancellation stopped working and is off for this "
                  "call. Others can still hear you.");
    return tr("The call ended unexpectedly.");
}

QString SfuCallController::joinRefusalMessage(const QString &block)
{
    if (block == QLatin1String("unsupported"))
        return tr("This build can't join Matrix calls.");
    if (block == QLatin1String("undiscovered"))
        return tr("Still checking whether calling is available here. Try "
                  "again in a moment.");
    if (block == QLatin1String("no_transport"))
        return tr("There's no Matrix calling service on this homeserver.");
    if (block == QLatin1String("discovery_failed"))
        return tr("Lightning couldn't check whether calling is available "
                  "here.");
    if (block == QLatin1String("session_closed"))
        return tr("This call has ended.");
    if (block == QLatin1String("no_media_transport"))
        return tr("This build has no calling media support.");
    if (block == QLatin1String("network_route"))
        return tr("Calls aren't available while this account uses a proxy.");
    if (block == QLatin1String("media_encryption_unavailable"))
        return tr("This room is encrypted, and encrypted calls aren't "
                  "available yet on this build.");
    if (block == QLatin1String("no_permission"))
        return tr("You don't have permission to start or join calls in this "
                  "room. A room admin can raise your power level in it.");
    return tr("This call can't be joined right now.");
}

bool SfuCallController::startsCallForAnnouncement(
    const QVariantList &participants)
{
    for (const QVariant &row : participants) {
        if (!row.toMap().value(QStringLiteral("ownDevice")).toBool())
            return false;
    }
    return true;
}

QString SfuCallController::announcementTypeFor(const RoomInfo &room)
{
    return calls::oneToOneDmPeer(room).isEmpty()
        ? QStringLiteral("notification")
        : QStringLiteral("ring");
}

void SfuCallController::refuseJoin(const QString &message, bool announce)
{
    if (active()) {
        qCWarning(lcSfuCall) << "join refused while a call is active; "
                                "keeping that call";
        m_failureAnnounced = true;
        m_announcedFailure = message;
        Q_EMIT callFailed(message);
        return;
    }
    setState(State::Failed, message);
    if (announce)
        Q_EMIT callFailed(m_lastError);
}

void SfuCallController::resetKeyLane()
{
    m_useKeyTimer.stop();
    m_keySwitchFallbackTimer.stop();
    m_unaddressableLogged = -1;
    m_newestKey.raw.fill('\0');
    m_newestKey = OutboundKey{};
    m_adoptedKeySerial = 0;
    m_adoptedKeyIndex = -1;
    m_keyRecipients.clear();
    m_keyHolders.clear();
    m_adoptedKeyRecipients.clear();
    m_deliveredKeyIndex = -1;
    m_keySendOps.clear();
    m_keyResendBackoff.clear();
    m_keyResendsHeldBack = 0;
}

void SfuCallController::noteKeyDeliveryFailed(const QString &device)
{
    KeyResendBackoff &backoff = m_keyResendBackoff[device];
    ++backoff.failures;
    qint64 gap = 0;
    if (backoff.failures >= 2) {
        const int shift = qMin(backoff.failures - 2, 16);
        gap = qMin<qint64>(qint64(m_keyResendBaseMs) << shift,
                           kKeyResendCeilingMs);
    }
    backoff.notBeforeMs = m_keyClock.elapsed() + gap;
}

bool SfuCallController::join(const QString &roomId, bool withVideo)
{
    if (roomId.isEmpty())
        return false;
    if (!m_client || !m_client->supportsSfu()) {
        refuseJoin(tr("This build can't join Matrix calls."), false);
        return false;
    }
#ifndef HAVE_LIGHTNING_WEBRTC
    refuseJoin(tr("This build has no calling media support."), false);
    return false;
#else
    if (m_engine.isNull()) {
        qCWarning(lcSfuCall) << "join refused: no media engine";
        refuseJoin(tr("This build has no calling media support."), false);
        return false;
    }
    if (!m_rtc) {
        qCWarning(lcSfuCall) << "join refused: no rtc controller";
        refuseJoin(tr("Calling isn't ready yet."), false);
        return false;
    }

    // Every join block refuses, and fails closed. An encrypted room whose
    // media cannot be encrypted must never be joined in the clear, and no
    // membership may be published for a session we cannot join.
    const QString block = m_rtc->joinBlockReason(roomId);
    if (!block.isEmpty()) {
        qCWarning(lcSfuCall) << "join refused: block=" << block;
        refuseJoin(joinRefusalMessage(block), true);
        return false;
    }

    if (active())
        teardown(State::Ended);

    m_rtc->forgetUnresolvedIdentityDiagnostics();

    qCInfo(lcSfuCall) << "join begin encrypted="
                      << m_rtc->roomEncrypted(roomId)
                      << "focus=" << (m_rtc->focusUrlFor(roomId).isEmpty()
                                      ? QStringLiteral("<none>")
                                      : QStringLiteral("<set>"));
    ++m_generation;
    ++m_deviceFallbackToken;
    m_waitingPortalReleaseCid.clear();
    m_releasedPortalCid.clear();
    stopReconnect();
    m_shareSourceReusable = false;
    m_transportEverConnected = false;
    m_transportStates.clear();
    m_reconnectEpisodeStarts.clear();
    m_roomId = roomId;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    m_cameraFellBack = false;
    m_cameraFellBackToPortal = false;
    m_screenSharing = false;
    m_handRaised = false;
    m_handReactionId.clear();
    m_handOp = 0;
    m_handReactions.clear();
    m_pendingAnnotations.clear();
    m_reactionOp = 0;
    m_lastReactionSentMs = 0;
    m_participants.clear();
    m_remoteTrackMuted.clear();
    m_publisherParticipants.clear();
    m_inboundRingKeys.clear();
    if (!m_blockedStreams.isEmpty()) {
        m_blockedStreams.clear();
        Q_EMIT remoteMediaBlockedChanged();
    }
    if (m_microphoneSilent) {
        m_microphoneSilent = false;
        Q_EMIT microphoneSilentChanged();
    }
    resetMicrophoneLevel();
    setNoiseSuppressionFailedMode(QString());
    expireParkedKeys();
    m_speaking.clear();
    m_speakingLevel.clear();
    m_connectionQuality.clear();
    if (m_participantModel)
        m_participantModel->clear();
    if (m_shareModel)
        m_shareModel->clear();
    if (m_stageState)
        m_stageState->clear();
    m_publishedTrackIds.clear();
    m_publishedTrackSids.clear();
    m_shareVolumes.clear();
    m_engineParticipantVolume.clear();
    m_engineShareVolume.clear();
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_membershipEventId.clear();
    m_membershipPublished = false;
    m_delayId.clear();
    m_delayedCategory.clear();
    m_ownIdentity.clear();
    m_mediaEncrypted = false;
    resetKeyLane();
    m_playbackLostAnnounced = false;
    m_cameraNoticeCategory.clear();
    m_cameraNoticeCid.clear();
    m_candidatesSent = 0;
    m_lastPublishMs = 0;
    m_membershipAcceptedMs = 0;
    m_membershipLongExpiryUnguarded = false;
    m_refreshOp = 0;
    m_delayedRestartOp = 0;
    supersedeOwedRetractionFor(roomId);

    m_roomEncrypted = m_rtc->roomEncrypted(roomId);
    m_engine->setEncryptionRequired(m_roomEncrypted);
    m_engine->clearKeys();

    ++m_callGeneration;
    m_publishFallbackTried = false;
    m_sfuGeneration = 0;
    m_connectionFocusUrls.clear();
    clearSubscriberEngines();
    m_focusUrl = m_rtc->focusUrlFor(roomId);

    setState(State::Preparing);
    Q_EMIT mediaStateChanged();
    Q_EMIT participantsChanged();

    m_announceOnPublish =
        m_rtc && startsCallForAnnouncement(m_rtc->participants(roomId));
    m_announceIntent = withVideo ? QStringLiteral("video")
                                 : QStringLiteral("audio");

    m_publishOp = m_client->rtcPublishMembership(
        roomId, m_focusUrl, withVideo ? QStringLiteral("video")
                                      : QStringLiteral("audio"),
        !m_rtc->ownFocusUrlFor().isEmpty()
            && RtcController::sameFocusUrl(m_focusUrl, m_rtc->ownFocusUrlFor())
            && (!m_rtc->usesLegacyFocusFor(roomId) || m_publishFallbackTried));
    qCInfo(lcSfuCall) << "membership publish op=" << m_publishOp;
    if (m_publishOp == 0) {
        teardown(State::Failed, tr("Couldn't announce you in the call."));
        Q_EMIT callFailed(m_lastError);
        return false;
    }
    return true;
#endif
}

namespace {
QString membershipRefusalCategory(const QString &category)
{
    if (category == QLatin1String("forbidden"))
        return QStringLiteral("membership_forbidden");
    return category;
}
}

namespace {
struct MembershipSettleGuard
{
    SfuCallController *controller;
    bool pendingBefore;
    explicit MembershipSettleGuard(SfuCallController *c)
        : controller(c), pendingBefore(c->membershipWritesPending())
    {
    }
    ~MembershipSettleGuard()
    {
        if (pendingBefore && !controller->membershipWritesPending())
            Q_EMIT controller->membershipWritesSettled();
    }
};
}

void SfuCallController::onMembershipPublished(quint64 opId, bool ok,
                                              const QString &category,
                                              const QString &eventId,
                                              const QString &delayId,
                                              const QString &delayedCategory)
{
    const MembershipSettleGuard settle(this);
    if (opId == 0)
        return;
    if (!delayId.isEmpty())
        m_delayedCategory.clear();
    else if (!delayedCategory.isEmpty())
        m_delayedCategory = delayedCategory;
    if (m_abandonedPublishOp != 0 && opId == m_abandonedPublishOp) {
        const QString room = m_abandonedPublishRoomId;
        m_abandonedPublishOp = 0;
        m_abandonedPublishRoomId.clear();
        if (!ok && eventId.isEmpty()) {
            qCInfo(lcSfuCall)
                << "an abandoned membership publish was refused; nothing to "
                   "retract category=" << category;
            return;
        }
        if (active() && room == m_roomId) {
            qCInfo(lcSfuCall)
                << "a membership publish from a previous join landed while "
                   "we are back in the same room; its state event has "
                   "already been replaced by this call's own";
            return;
        }
        qCWarning(lcSfuCall)
            << "a membership publish landed AFTER we left; retracting it "
               "delayed=" << !delayId.isEmpty()
            << "delayed_reason=" << m_delayedCategory;
        startRetraction(room, delayId, QDateTime::currentMSecsSinceEpoch(),
                        !ok && delayId.isEmpty());
        return;
    }
    if (m_refreshOp != 0 && opId == m_refreshOp) {
        m_refreshOp = 0;
        if (!active())
            return;
        if (!ok) {
            qCWarning(lcSfuCall)
                << "membership refresh FAILED category=" << category;
            if (!eventId.isEmpty() && delayId.isEmpty()) {
                m_membershipAcceptedMs = QDateTime::currentMSecsSinceEpoch();
                m_membershipLongExpiryUnguarded = true;
            }
            return;
        }
        m_membershipPublished = true;
        m_membershipAcceptedMs = QDateTime::currentMSecsSinceEpoch();
        m_membershipLongExpiryUnguarded = false;
        m_delayId = delayId;
        qCInfo(lcSfuCall) << "membership refreshed delayed="
                          << !delayId.isEmpty()
                          << "delayed_reason=" << m_delayedCategory;
        return;
    }
    if (opId != m_publishOp)
        return;
    m_publishOp = 0;
    qCInfo(lcSfuCall) << "membership published ok=" << ok
                      << "category=" << category
                      << "delayed=" << !delayId.isEmpty()
                      << "delayed_reason=" << m_delayedCategory;
    if (ok || !eventId.isEmpty()) {
        m_membershipPublished = true;
        m_membershipAcceptedMs = QDateTime::currentMSecsSinceEpoch();
        m_membershipLongExpiryUnguarded = !ok && delayId.isEmpty();
    }
    if (m_state != State::Preparing)
        return;
    if (!ok) {
        const QString reported = membershipRefusalCategory(category);
        qCWarning(lcSfuCall)
            << "membership REFUSED by the homeserver category=" << category
            << "reportedAs=" << reported;
        const QString refusedRoom = m_roomId;
        teardown(State::Failed, userFacingError(reported));
        Q_EMIT callFailed(m_lastError);
        if (m_rtc)
            m_rtc->noteMembershipRefused(refusedRoom, category);
        return;
    }
    m_membershipEventId = eventId;

    if (m_announceOnPublish && m_client && !m_membershipEventId.isEmpty()) {
        m_announceOnPublish = false;
        const QString type =
            announcementTypeFor(m_client->roomInfo(m_roomId));
        const quint64 op = m_client->rtcNotify(
            m_roomId, type, m_announceIntent, kAnnounceLifetimeMs,
            m_membershipEventId);
        const QString peer =
            calls::oneToOneDmPeer(m_client->roomInfo(m_roomId));
        if (!peer.isEmpty()) {
            m_announceOp = op;
            m_announcePeer = peer;
        }
        qCInfo(lcSfuCall) << "call announced to the room op=" << op
                          << "intent=" << m_announceIntent
                          << "type=" << type;
    }
    m_delayId = delayId;
    if (delayId.isEmpty()) {
        qCWarning(lcSfuCall)
            << "no MSC4140 delayed retraction armed — an unclean exit will "
               "leave this membership until it expires";
    }
    m_lastPublishMs = QDateTime::currentMSecsSinceEpoch();
    m_refreshTimer.start();

    if (m_client && !m_roomId.isEmpty())
        m_client->rtcReadRaisedHands(m_roomId);

    if (m_focusUrl.isEmpty()) {
        teardown(State::Failed,
                 tr("Calling isn't available on this homeserver."));
        Q_EMIT callFailed(m_lastError);
        return;
    }
    setState(State::Authorizing);
    const quint64 connectOp = connectFocusPlan();
    qCInfo(lcSfuCall) << "sfu connect op=" << connectOp;
    if (connectOp == 0) {
        teardown(State::Failed, tr("Couldn't connect to the call."));
        Q_EMIT callFailed(m_lastError);
    }
}

void SfuCallController::clearSubscriberEngines()
{
    m_subscriberParticipants.clear();
    m_connectionOwnIdentity.clear();
#ifdef HAVE_LIGHTNING_WEBRTC
    const auto engines = m_subscriberEngines;
    m_subscriberEngines.clear();
    for (const auto &engine : engines) {
        if (!engine.isNull()) {
            engine->stop();
            engine->deleteLater();
        }
    }
#else
    m_subscriberEngines.clear();
#endif
}

void SfuCallController::logRemoteSubscription(
    int id, const QString &sid, const QString &trackSid, const QString &kind) const
{
    const QString routedSid = id == 0 ? sid
        : QStringLiteral("c%1:").arg(id) + sid;
    const auto connectionRows = id == 0 ? m_publisherParticipants
        : m_subscriberParticipants.value(id);
    for (const QVariantMap &row : connectionRows) {
        if (row.value(QStringLiteral("sid")).toString() != routedSid)
            continue;
        const QString identity = row.value(QStringLiteral("identity")).toString();
        if (identity.isEmpty())
            break;
        bool trackMatched = false;
        for (const QVariant &trackValue : row.value(QStringLiteral("tracks")).toList()) {
            const QString knownSid = trackValue.toMap()
                .value(QStringLiteral("sid")).toString();
            const QString routedTrackSid = id == 0 ? trackSid
                : QStringLiteral("c%1:").arg(id) + trackSid;
            if (!trackSid.isEmpty() && knownSid == routedTrackSid) {
                trackMatched = true;
                break;
            }
        }
        const QByteArray hash = QCryptographicHash::hash(
            identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
        qCInfo(lcSfuCall) << "remote receive pad installed"
            << "connection=" << id << "participantHash=" << hash
            << "trackMatched=" << trackMatched << "kind=" << kind;
        return;
    }
    qCInfo(lcSfuCall) << "remote receive pad unattributed"
                      << "connection=" << id << "kind=" << kind;
}

void SfuCallController::updateSubscriberParticipants(
    int id, const QVariantList &rows, bool replace)
{
    if (replace)
        m_subscriberParticipants.remove(id);
    auto &known = m_subscriberParticipants[id];
    QVariantList forwarded;
    for (const QVariant &value : rows) {
        QVariantMap row = value.toMap();
        const QString identity = row.value(QStringLiteral("identity")).toString();
        const QString expectedOwn = m_client
            ? m_client->currentUserId() + QLatin1Char(':')
                + m_client->currentDeviceId() : QString();
        if (identity.isEmpty() || identity == m_connectionOwnIdentity.value(id)
            || identity == m_ownIdentity || identity == expectedOwn)
            continue;
        if (row.value(QStringLiteral("state")).toString()
            == QLatin1String("disconnected")) {
            known.remove(identity);
        } else if (known.size() < kMaxParticipants || known.contains(identity)) {
            const QString prefix = QStringLiteral("c%1:").arg(id);
            const QString sid = row.value(QStringLiteral("sid")).toString();
            if (!sid.isEmpty())
                row.insert(QStringLiteral("sid"), prefix + sid);
            QVariantList tracks;
            for (const QVariant &trackValue : row.value(QStringLiteral("tracks")).toList()) {
                QVariantMap track = trackValue.toMap();
                const QString trackSid = track.value(QStringLiteral("sid")).toString();
                if (!trackSid.isEmpty())
                    track.insert(QStringLiteral("sid"), prefix + trackSid);
                tracks.append(track);
            }
            row.insert(QStringLiteral("tracks"), tracks);
            known.insert(identity, row);
        } else {
            continue;
        }
        if (row.value(QStringLiteral("state")).toString()
            != QLatin1String("disconnected"))
            forwarded.append(row);
    }
    noteRemoteTrackMutes(forwarded);
    onSfuParticipants({});
}

quint64 SfuCallController::connectFocusPlan()
{
    if (!m_client || m_focusUrl.isEmpty())
        return 0;
    clearSubscriberEngines();
    m_sfuGeneration = 0;
    m_connectionFocusUrls = {m_focusUrl};
    for (const QString &url : m_rtc ? m_rtc->subscriberFocusUrlsFor(m_roomId)
                                    : QStringList()) {
        if (!containsFocusUrl(m_connectionFocusUrls, url)
            && m_connectionFocusUrls.size() < 8)
            m_connectionFocusUrls.append(url);
    }
#ifdef HAVE_LIGHTNING_WEBRTC
    for (int id = 1; id < m_connectionFocusUrls.size(); ++id) {
        auto *engine = new SfuMediaEngine(this);
        m_subscriberEngines.insert(id, engine);
        engine->setVideoRouter(m_videoRouter);
        engine->setReceiveNamespace(QStringLiteral("c%1:").arg(id));
        engine->setEncryptionRequired(m_roomEncrypted);
        if (!m_engine.isNull()) {
            engine->setPreferredDevices(m_engine->cameraChoice(),
                                        m_engine->microphoneChoice(),
                                        m_engine->speakerChoice());
            engine->setTestSourceMode(m_engine->testSourceMode());
        }
        engine->setOutputMuted(m_deafened);
        for (auto ring = m_inboundRingKeys.cbegin();
             ring != m_inboundRingKeys.cend(); ++ring) {
            for (auto key = ring.value().cbegin(); key != ring.value().cend(); ++key)
                engine->setInboundKey(ring.key(), key.key(), key.value());
        }
        const quint64 callGeneration = m_callGeneration;
        connect(engine, &SfuMediaEngine::localDescription, this,
                [this, engine, id, callGeneration](int target,
                    const QString &kind, const QString &sdp) {
                    if (!m_client || m_callGeneration != callGeneration
                        || m_subscriberEngines.value(id) != engine)
                        return;
                    m_client->sfuLocalDescriptionFor(
                        m_sfuGeneration, id, kind,
                        target == 0 ? QStringLiteral("publisher")
                                    : QStringLiteral("subscriber"), sdp);
                });
        connect(engine, &SfuMediaEngine::localCandidate, this,
                [this, engine, id, callGeneration](int target,
                    const QString &candidate) {
                    if (!m_client || m_callGeneration != callGeneration
                        || m_subscriberEngines.value(id) != engine)
                        return;
                    m_client->sfuLocalCandidateFor(
                        m_sfuGeneration, id,
                        target == 0 ? QStringLiteral("publisher")
                                    : QStringLiteral("subscriber"), candidate);
                });
        connect(engine, &SfuMediaEngine::remoteTrackAdded, this,
                [this, id](const QString &sid, const QString &mid, const QString &kind) {
                    logRemoteSubscription(id, sid, mid, kind);
                });
        connect(engine, &SfuMediaEngine::failed, this,
                [this, engine, id, callGeneration](const QString &category) {
                    if (m_callGeneration == callGeneration
                        && m_subscriberEngines.value(id) == engine && active())
                        beginReconnect(category);
                });
        connect(engine, &SfuMediaEngine::transportStateChanged, this,
                [this, engine, id, callGeneration](int target,
                                                   const QString &state) {
                    if (m_callGeneration != callGeneration
                        || m_subscriberEngines.value(id) != engine || !active())
                        return;
                    if (target == 1) {
                        qCInfo(lcSfuCall) << "subscriber transport"
                                          << "connection=" << id
                                          << "state=" << state;
                        if (state == QLatin1String("failed"))
                            beginReconnect(QStringLiteral("subscriber_ice_failed"));
                    }
                });
    }
#endif
    return m_client->sfuConnectMany(m_connectionFocusUrls, m_roomId);
}

void SfuCallController::onSfuConnectionEvent(const QVariantMap &event)
{
    if (!active())
        return;
    const quint64 generation = event.value(QStringLiteral("generation")).toULongLong();
    const int id = event.contains(QStringLiteral("connection_id"))
        ? event.value(QStringLiteral("connection_id")).toInt() : -1;
    if (id < 0 || id >= m_connectionFocusUrls.size() || generation == 0)
        return;
    if (m_sfuGeneration == 0)
        m_sfuGeneration = generation;
    if (generation != m_sfuGeneration || id == 0)
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    auto *engine = m_subscriberEngines.value(id).data();
    if (!engine)
        return;
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("sfu_state")) {
        const QString state = event.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("failed")
            || state == QLatin1String("reconnecting")
            || state == QLatin1String("closed")
            || state == QLatin1String("ended")) {
            onSfuState(state, event.value(QStringLiteral("category")).toString());
        }
        return;
    }
    if (type == QLatin1String("sfu_joined")) {
        engine->start();
        engine->setOutputMuted(m_deafened);
        const QByteArray trailer = QByteArray::fromBase64(
            event.value(QStringLiteral("sif_trailer")).toString().toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        engine->setServerInjectedTrailer(trailer);
        engine->setIceServers(event.value(QStringLiteral("ice_servers")).toList());
        const QString self = event.value(QStringLiteral("identity")).toString();
        m_connectionOwnIdentity.insert(id, self);
        updateSubscriberParticipants(
            id, event.value(QStringLiteral("participants")).toList(), true);
        noteParticipantIdentities();
        return;
    }
    if (type == QLatin1String("sfu_participants")) {
        updateSubscriberParticipants(
            id, event.value(QStringLiteral("participants")).toList(), false);
    } else if (type == QLatin1String("sfu_remote_description")) {
        if (event.value(QStringLiteral("target")).toString()
            == QLatin1String("subscriber")) {
            engine->applyRemoteDescription(SfuMediaEngine::Target::Subscriber,
                event.value(QStringLiteral("kind")).toString(),
                event.value(QStringLiteral("sdp")).toString());
        }
    } else if (type == QLatin1String("sfu_remote_candidate")) {
        if (event.value(QStringLiteral("target")).toString()
            == QLatin1String("subscriber")) {
            engine->applyRemoteCandidate(SfuMediaEngine::Target::Subscriber,
                event.value(QStringLiteral("candidate_init")).toString());
        }
    } else if (type == QLatin1String("sfu_speakers")) {
        QVariantList rows;
        for (const QVariant &value : event.value(QStringLiteral("speakers")).toList()) {
            QVariantMap row = value.toMap();
            const QString sid = row.value(QStringLiteral("sid")).toString();
            if (!sid.isEmpty())
                row.insert(QStringLiteral("sid"), QStringLiteral("c%1:").arg(id) + sid);
            rows.append(row);
        }
        onSfuSpeakers(rows);
    } else if (type == QLatin1String("sfu_quality")) {
        QVariantList rows;
        for (const QVariant &value : event.value(QStringLiteral("updates")).toList()) {
            QVariantMap row = value.toMap();
            const QString sid = row.value(QStringLiteral("sid")).toString();
            if (!sid.isEmpty())
                row.insert(QStringLiteral("sid"), QStringLiteral("c%1:").arg(id) + sid);
            rows.append(row);
        }
        onSfuConnectionQuality(rows);
    }
#else
    Q_UNUSED(event);
#endif
}

void SfuCallController::onSfuState(const QString &state,
                                    const QString &category)
{
    qCInfo(lcSfuCall) << "sfu state=" << state << "category=" << category
                      << "active=" << active();
    if (!active())
        return;
    // The signalling task is reattaching the same LiveKit session; tearing the
    // media down here would turn a resume into the full rejoin it avoids.
    if (state == QLatin1String("resume_requested")
        || state == QLatin1String("resuming")) {
        return;
    }
    if (state == QLatin1String("resumed")) {
        onSignalResumed();
        return;
    }
    if (state == QLatin1String("failed")
        && category == QLatin1String("publish_denied")
        && !m_publishFallbackTried && m_rtc && m_client) {
        const QString own = m_rtc->ownFocusUrlFor();
        if (!own.isEmpty() && !RtcController::sameFocusUrl(own, m_focusUrl)) {
            m_publishFallbackTried = true;
            m_client->sfuDisconnect();
            m_focusUrl = own;
            republishMembership();
            const quint64 retryOp = connectFocusPlan();
            qCInfo(lcSfuCall) << "publisher moved to own focus after denial"
                               << "retryOp=" << retryOp;
            if (retryOp != 0)
                return;
        }
    }
    if (m_state == State::Reconnecting) {
        onReconnectSfuState(state, category);
        return;
    }
    if (m_state == State::Connected
        && (state == QLatin1String("reconnecting")
            || state == QLatin1String("closed")
            || (state == QLatin1String("failed")
                && !sfuFailureIsRefusal(category)))) {
        beginReconnect(category.isEmpty() ? state : category);
        return;
    }
    if (state == QLatin1String("reconnecting")) {
        if (m_state == State::Connecting) {
            teardown(State::Failed, connectionLostMessage());
            Q_EMIT callFailed(m_lastError);
        }
        return;
    }
    if (state == QLatin1String("authorized")) {
        setState(State::Connecting);
        return;
    }
    if (state == QLatin1String("signalling")) {
        setState(State::Connecting);
        return;
    }
    if (state == QLatin1String("failed")) {
        const bool serviceAnswered =
            category == QLatin1String("forbidden")
            || category == QLatin1String("publish_denied")
            || category == QLatin1String("sfu_forbidden")
            || category == QLatin1String("unsupported")
            || category == QLatin1String("sfu_not_found")
            || category == QLatin1String("rate_limited")
            || category == QLatin1String("server_error")
            || category == QLatin1String("ws_rejected")
            || category == QLatin1String("membership_forbidden");
        if (serviceAnswered) {
            qCWarning(lcSfuCall)
                << "the call SERVICE refused this call category=" << category;
        } else {
            qCWarning(lcSfuCall)
                << "this call could not reach its service category="
                << category;
        }
        teardown(State::Failed, userFacingError(category));
        Q_EMIT callFailed(m_lastError);
        return;
    }
    if (state == QLatin1String("ended") || state == QLatin1String("closed")) {
        if (m_state == State::Connected || m_state == State::Connecting) {
            teardown(State::Failed, connectionLostMessage());
            Q_EMIT callFailed(m_lastError);
        }
    }
}

QString SfuCallController::connectionLostMessage() const
{
    if (!m_transportEverConnected)
        return tr("Couldn't connect to the call.");
    return tr("The call ended because the connection was lost.");
}

QString SfuCallController::shareWhileReconnectingMessage()
{
    return tr("You can share your screen once the call has reconnected.");
}

bool SfuCallController::admitReconnectEpisode()
{
    const qint64 now = m_episodeClock.elapsed();
    while (!m_reconnectEpisodeStarts.isEmpty()
           && now - m_reconnectEpisodeStarts.constFirst()
                  >= m_reconnectEpisodeWindowMs) {
        m_reconnectEpisodeStarts.removeFirst();
    }
    if (m_reconnectEpisodeStarts.size() >= m_maxReconnectEpisodes)
        return false;
    m_reconnectEpisodeStarts.append(now);
    return true;
}

bool SfuCallController::sfuFailureIsRefusal(const QString &category)
{
    static const QSet<QString> refusals = {
        QStringLiteral("forbidden"),
        QStringLiteral("sfu_forbidden"),
        QStringLiteral("publish_denied"),
        QStringLiteral("membership_forbidden"),
        QStringLiteral("unsupported"),
        QStringLiteral("sfu_not_found"),
        QStringLiteral("focus_private_name"),
        QStringLiteral("focus_unroutable"),
        QStringLiteral("focus_url_invalid"),
    };
    return refusals.contains(category);
}

void SfuCallController::beginReconnect(const QString &reason)
{
    if (!active() || !m_client || m_roomId.isEmpty()
        || m_focusUrl.isEmpty()) {
        if (active()) {
            teardown(State::Failed, connectionLostMessage());
            Q_EMIT callFailed(m_lastError);
        }
        return;
    }
    const bool fresh = m_state != State::Reconnecting;
    if (fresh && !admitReconnectEpisode()) {
        qCWarning(lcSfuCall)
            << "call lost its session again; reconnect episodes exhausted"
            << "max=" << m_maxReconnectEpisodes
            << "windowMs=" << m_reconnectEpisodeWindowMs
            << "reason=" << reason;
        teardown(State::Failed, connectionLostMessage());
        Q_EMIT callFailed(m_lastError);
        return;
    }
    qCWarning(lcSfuCall) << "call RECONNECTING reason=" << reason
                         << "fresh=" << fresh
                         << "attempts=" << m_reconnectAttempts;
    m_iceDisconnectedTimer.stop();
    if (fresh) {
        m_reconnectAttempts = 0;
        m_reconnectClock.start();
        m_reconnectDeadlineTimer.start();
        setState(State::Reconnecting);
        if (m_portal && m_portal->busy())
            m_portal->cancel();
        if (!m_screenShareSources.isEmpty()) {
            m_screenShareSources.clear();
            Q_EMIT screenShareSourcesChanged();
        }
    }
    suspendSfuSession();
    scheduleReconnectAttempt();
}

void SfuCallController::suspendSfuSession()
{
    m_reconnectAttemptTimer.stop();
    m_reconnectPhase = ReconnectPhase::Waiting;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull())
        m_engine->suspend();
    clearSubscriberEngines();
#endif
    m_sfuGeneration = 0;
    m_connectionFocusUrls.clear();
    m_transportStates.clear();
    m_resumeSnapshotPending = false;
    if (m_client)
        m_client->sfuDisconnect();
    if (m_cameraAwaitingPortal) {
        m_cameraAwaitingPortal = false;
        if (m_cameraPortal)
            m_cameraPortal->cancel();
    }
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_shareAudioCid.clear();
    resetShareAudioLevel();
    m_publishedTrackIds.clear();
    m_publishedTrackSids.clear();
    m_candidatesSent = 0;
    if (m_screenSharing && !m_shareSourceReusable) {
        qCInfo(lcSfuCall) << "screen share ended by the reconnect (a portal "
                             "share cannot be resumed)";
        m_screenSharing = false;
        shareAudioShareEnded();
        if (m_portal)
            m_portal->cancel();
#ifdef HAVE_LIGHTNING_WEBRTC
        clearLocalVideoSurface(SfuMediaEngine::localScreenStreamId());
#endif
        Q_EMIT mediaStateChanged();
    }
    if (!m_blockedStreams.isEmpty()) {
        m_blockedStreams.clear();
        Q_EMIT remoteMediaBlockedChanged();
        Q_EMIT participantsChanged();
    }
    resetMicrophoneLevel();
}

void SfuCallController::scheduleReconnectAttempt()
{
    if (m_state != State::Reconnecting)
        return;
    int delay = m_reconnectFirstBackoffMs;
    for (int i = 0; i < m_reconnectAttempts && delay < m_reconnectMaxBackoffMs;
         ++i) {
        delay *= 2;
    }
    delay = qMin(delay, m_reconnectMaxBackoffMs);
    const qint64 left = m_reconnectDeadlineTimer.remainingTime();
    if (left >= 0 && delay >= left) {
        qCWarning(lcSfuCall) << "no time left for another reconnect attempt"
                             << "attempts=" << m_reconnectAttempts;
        giveUpReconnect(connectionLostMessage());
        return;
    }
    m_reconnectPhase = ReconnectPhase::Waiting;
    m_reconnectTimer.start(delay);
    qCInfo(lcSfuCall) << "reconnect attempt" << (m_reconnectAttempts + 1)
                      << "in" << delay << "ms";
}

void SfuCallController::launchReconnectAttempt()
{
    if (m_state != State::Reconnecting
        || m_reconnectPhase != ReconnectPhase::Waiting || !m_client) {
        return;
    }
    ++m_reconnectAttempts;
    m_reconnectPhase = ReconnectPhase::Dialing;
    m_reconnectAttemptTimer.start();
    const quint64 op = connectFocusPlan();
    qCInfo(lcSfuCall) << "reconnect attempt" << m_reconnectAttempts
                      << "dispatched op=" << op
                      << "elapsedMs=" << m_reconnectClock.elapsed();
    if (op == 0)
        reconnectAttemptFailed(QStringLiteral("dispatch_failed"));
}

void SfuCallController::reconnectAttemptFailed(const QString &why)
{
    if (m_state != State::Reconnecting
        || m_reconnectPhase == ReconnectPhase::Waiting) {
        return;
    }
    qCWarning(lcSfuCall) << "reconnect attempt" << m_reconnectAttempts
                         << "failed why=" << why
                         << "elapsedMs=" << m_reconnectClock.elapsed();
    suspendSfuSession();
    scheduleReconnectAttempt();
}

void SfuCallController::finishReconnect()
{
    qCInfo(lcSfuCall) << "call RECONNECTED attempts=" << m_reconnectAttempts
                      << "elapsedMs=" << m_reconnectClock.elapsed();
    stopReconnect();
    setState(State::Connected);
}

void SfuCallController::giveUpReconnect(const QString &error)
{
    if (m_state != State::Reconnecting)
        return;
    teardown(State::Failed, error);
    Q_EMIT callFailed(m_lastError);
}

void SfuCallController::stopReconnect()
{
    m_reconnectTimer.stop();
    m_reconnectAttemptTimer.stop();
    m_reconnectDeadlineTimer.stop();
    m_iceDisconnectedTimer.stop();
    m_reconnectPhase = ReconnectPhase::None;
    m_reconnectAttempts = 0;
}

void SfuCallController::onReconnectSfuState(const QString &state,
                                            const QString &category)
{
    const bool dialing = m_reconnectPhase == ReconnectPhase::Dialing;
    const bool live = m_reconnectPhase == ReconnectPhase::Signalling
        || m_reconnectPhase == ReconnectPhase::Joined;
    if (state == QLatin1String("authorized")) {
        return;
    }
    if (state == QLatin1String("signalling")) {
        if (dialing)
            m_reconnectPhase = ReconnectPhase::Signalling;
        return;
    }
    if (state == QLatin1String("failed")) {
        if (!dialing && !live) {
            qCInfo(lcSfuCall) << "stale sfu failure ignored while waiting to"
                                 " reconnect category=" << category;
            return;
        }
        if (sfuFailureIsRefusal(category)) {
            qCWarning(lcSfuCall)
                << "the call service refused the reconnect category="
                << category;
            giveUpReconnect(userFacingError(category));
            return;
        }
        reconnectAttemptFailed(category);
        return;
    }
    if (state == QLatin1String("reconnecting")
        || state == QLatin1String("closed")
        || state == QLatin1String("ended")) {
        if (!live) {
            qCInfo(lcSfuCall) << "stale sfu report ignored while reconnecting"
                              << "state=" << state;
            return;
        }
        if (state == QLatin1String("ended")) {
            giveUpReconnect(connectionLostMessage());
            return;
        }
        reconnectAttemptFailed(category.isEmpty() ? state : category);
        return;
    }
}

void SfuCallController::onSignalResumed()
{
    // Whatever the old socket was carrying is lost: an offer whose answer never
    // came would block every later renegotiation, and a mute may not have landed.
    m_resumeSnapshotPending = true;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull())
        m_engine->renegotiateAfterResume();
#endif
    syncMicMuteToSfu();
    // The server ICE-restarts only the subscriber; a publisher that is not
    // connected now gets the grace period and then the full rejoin.
    if (m_state == State::Connected
        && m_transportStates.value(0) != QLatin1String("connected")
        && !m_iceDisconnectedTimer.isActive()) {
        m_iceDisconnectedTimer.start();
    }
}

void SfuCallController::onEngineTransportState(int target,
                                               const QString &state)
{
    qCInfo(lcSfuCall) << "transport target=" << target << "state=" << state
                      << "callState=" << static_cast<int>(m_state);
    if (!active())
        return;
    m_transportStates.insert(target, state);
    if (state == QLatin1String("connected")) {
        const bool allConnected = std::all_of(
            m_transportStates.cbegin(), m_transportStates.cend(),
            [](const QString &s) { return s == QLatin1String("connected"); });
        if (allConnected)
            m_iceDisconnectedTimer.stop();
        m_transportEverConnected = true;
        if (m_state == State::Reconnecting
            && m_reconnectPhase == ReconnectPhase::Joined) {
            finishReconnect();
        }
        return;
    }
    if (state == QLatin1String("failed")) {
        if (m_state == State::Connected) {
            beginReconnect(QStringLiteral("ice_failed"));
        } else if (m_state == State::Reconnecting
                   && m_reconnectPhase == ReconnectPhase::Joined) {
            reconnectAttemptFailed(QStringLiteral("ice_failed"));
        }
        return;
    }
    if (state == QLatin1String("disconnected")) {
        if (m_state == State::Connected && !m_iceDisconnectedTimer.isActive())
            m_iceDisconnectedTimer.start();
        return;
    }
}

void SfuCallController::setReconnectTimingForTest(int firstBackoffMs,
                                                  int attemptTimeoutMs,
                                                  int budgetMs,
                                                  int iceDisconnectedGraceMs)
{
    m_reconnectFirstBackoffMs = qMax(1, firstBackoffMs);
    m_reconnectMaxBackoffMs = qMax(m_reconnectFirstBackoffMs,
                                   firstBackoffMs * 8);
    m_reconnectAttemptTimer.setInterval(qMax(1, attemptTimeoutMs));
    m_reconnectDeadlineTimer.setInterval(qMax(1, budgetMs));
    if (iceDisconnectedGraceMs >= 0)
        m_iceDisconnectedTimer.setInterval(qMax(1, iceDisconnectedGraceMs));
}

void SfuCallController::onSfuJoined(const QString &identity,
                                     const QVariantList &participants,
                                     const QVariantList &iceServers,
                                     const QByteArray &sifTrailer)
{
    qCInfo(lcSfuCall) << "sfu joined inCall=" << participants.size()
                      << "iceServers=" << iceServers.size()
                      << "identity=" << (identity.isEmpty()
                                         ? QStringLiteral("<empty>")
                                         : QStringLiteral("<set>"))
                      << "sifTrailerLen=" << sifTrailer.size()
                      << "active=" << active();
    if (!active())
        return;
    const bool rejoin = m_state == State::Reconnecting;
    if (rejoin && m_reconnectPhase != ReconnectPhase::Dialing
        && m_reconnectPhase != ReconnectPhase::Signalling) {
        qCInfo(lcSfuCall) << "stale sfu join ignored while reconnecting";
        return;
    }
    m_ownIdentity = identity;
    noteRemoteTrackMutes(participants);
    m_participants = participants.mid(0, kMaxParticipants);
    m_publisherParticipants.clear();
    for (const QVariant &value : std::as_const(m_participants)) {
        const QVariantMap row = value.toMap();
        const QString identity = row.value(QStringLiteral("identity")).toString();
        if (!identity.isEmpty())
            m_publisherParticipants.insert(identity, row);
    }
    mergeParticipants({});
    noteParticipantIdentities();
    rebuildModels();
    if (rejoin) {
        m_reconnectPhase = ReconnectPhase::Joined;
        m_keyHolders.clear();
        qCInfo(lcSfuCall) << "reconnect attempt" << m_reconnectAttempts
                          << "joined inCall=" << m_participants.size()
                          << "elapsedMs=" << m_reconnectClock.elapsed();
    }
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull()) {
        m_engine->start();
        m_engine->setServerInjectedTrailer(sifTrailer);
        m_engine->setIceServers(iceServers);
        applyAudioState();
        publishTracks();
        if (rejoin && m_screenSharing && m_shareSourceReusable) {
            m_screenSharing = false;
            if (!startScreenShare(m_shareNodeId, -1, m_shareWindowHandle,
                                  m_shareCaptureRect)) {
                qCWarning(lcSfuCall)
                    << "screen share could not be resumed after the "
                       "reconnect";
                Q_EMIT mediaStateChanged();
            }
        }
    }
#else
    Q_UNUSED(iceServers); Q_UNUSED(sifTrailer);
#endif
    if (!rejoin)
        setState(State::Connecting);
    else
        distributeKeyIfNeeded();
    applyParkedKeys();
    Q_EMIT participantsChanged();
}

void SfuCallController::publishTracks()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_client) {
        qCWarning(lcSfuCall) << "publishTracks skipped: engine or client gone";
        return;
    }
    qCInfo(lcSfuCall) << "publishTracks camera=" << m_cameraOn
                      << "encrypted=" << m_roomEncrypted;
    if (m_roomEncrypted)
        startKeyLane();
    const QString audioCid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_audioCid = audioCid;
    m_publishedTrackIds.append(audioCid);
    m_client->sfuAddTrackFor(m_sfuGeneration, 0, audioCid, QStringLiteral("microphone"), 0,
0,0, false, m_roomEncrypted);
    m_engine->publishAudio(audioCid);
    QTimer::singleShot(8000, this, [this, audioCid] {
        if (!active() || m_audioCid != audioCid)
            return;
        if (m_publishedTrackSids.contains(audioCid))
            return;
        qCWarning(lcSfuCall)
            << "THE SFU NEVER CONFIRMED OUR MICROPHONE TRACK: it was declared"
            << "8s ago and no TrackPublished has come back, so nothing we"
            << "encode is being forwarded to anyone and every local counter"
            << "will still say the call is healthy.";
    });

    if (m_cameraOn)
        startCameraCapture();
#endif
}

void SfuCallController::onSfuParticipants(const QVariantList &updates)
{
    if (!sfuSessionLive())
        return;
    noteRemoteTrackMutes(updates);
    QSet<QString> present;
    for (const QVariant &row : updates)
        present.insert(row.toMap().value(QStringLiteral("identity")).toString());
    // The resume snapshot lists everyone, us included; a one-participant
    // broadcast that lands first must not drop the rest.
    if (m_resumeSnapshotPending && present.contains(m_ownIdentity)) {
        m_resumeSnapshotPending = false;
        for (auto it = m_publisherParticipants.begin(); it != m_publisherParticipants.end();) {
            if (present.contains(it.key()))
                ++it;
            else
                it = m_publisherParticipants.erase(it);
        }
    }
    const bool setChanged = mergeParticipants(updates);
    if (setChanged && !m_blockedStreams.isEmpty()) {
        const bool had = true;
        QSet<QString> live;
        for (const QVariant &entry : m_participants) {
            const QString sid =
                streamIdForIdentity(entry.toMap()
                                        .value(QStringLiteral("identity"))
                                        .toString());
            if (!sid.isEmpty())
                live.insert(sid);
        }
        const int before = m_blockedStreams.size();
        m_blockedStreams.intersect(live);
        if (m_blockedStreams.size() != before) {
            if (had != !m_blockedStreams.isEmpty())
                Q_EMIT remoteMediaBlockedChanged();
            Q_EMIT participantsChanged();
        }
    }
    distributeKeyIfNeeded();
    syncMicMuteToSfu();
    applyVideoState();
    if (m_rtc && !m_roomId.isEmpty())
        m_rtc->refresh(m_roomId);
}

void SfuCallController::noteRemoteTrackMutes(const QVariantList &updates)
{
    constexpr int kMaxTrackedTracks = kMaxParticipants * 8;
    for (const QVariant &value : updates) {
        const QVariantMap row = value.toMap();
        const QString identity =
            row.value(QStringLiteral("identity")).toString();
        if (identity.isEmpty() || identity == m_ownIdentity)
            continue;
        const QString stream = row.value(QStringLiteral("sid")).toString();
        const QVariantList tracks = row.value(QStringLiteral("tracks")).toList();
        for (const QVariant &trackValue : tracks) {
            const QVariantMap track = trackValue.toMap();
            const QString trackSid = track.value(QStringLiteral("sid")).toString();
            if (trackSid.isEmpty())
                continue;
            const bool muted = track.value(QStringLiteral("muted")).toBool();
            auto it = m_remoteTrackMuted.find(trackSid);
            if (it == m_remoteTrackMuted.end()) {
                if (m_remoteTrackMuted.size() >= kMaxTrackedTracks)
                    continue;
                it = m_remoteTrackMuted.insert(trackSid,
                                               RemoteTrackMute{muted, 0});
                if (!muted)
                    continue;
            } else if (it->muted == muted) {
                continue;
            }
            it->muted = muted;
            ++it->changes;
            if (it->changes > 5 && it->changes % 50 != 0)
                continue;
            qCInfo(lcSfuCall)
                << "remote track mute stream=" << stream << "track="
                << trackSid << "kind="
                << track.value(QStringLiteral("kind")).toString()
                << "muted=" << muted << "change=" << it->changes;
        }
    }
}

bool SfuCallController::mergeParticipants(const QVariantList &updates)
{
    QSet<QString> before;
    for (const QVariant &row : std::as_const(m_participants)) {
        before.insert(
            row.toMap().value(QStringLiteral("identity")).toString());
    }
    for (const QVariant &value : updates) {
        const QVariantMap row = value.toMap();
        const QString identity =
            row.value(QStringLiteral("identity")).toString();
        if (identity.isEmpty())
            continue;
        const QString sid = row.value(QStringLiteral("sid")).toString();
        const QString knownSid = m_publisherParticipants.value(identity)
            .value(QStringLiteral("sid")).toString();
        if (row.value(QStringLiteral("state")).toString()
            == QLatin1String("disconnected")) {
            if (m_publisherParticipants.remove(identity) > 0)
                qCInfo(lcSfuCall) << "sfu participant left stream=" << sid;
            continue;
        }
        if (!knownSid.isEmpty() && !sid.isEmpty() && knownSid != sid) {
            qCWarning(lcSfuCall) << "sfu participant session replaced stream="
                                 << knownSid << "->" << sid;
        }
        if (m_publisherParticipants.size() < kMaxParticipants
            || m_publisherParticipants.contains(identity))
            m_publisherParticipants.insert(identity, row);
    }
    QMap<QString, QVariantMap> visible;
    for (auto it = m_publisherParticipants.cbegin();
         it != m_publisherParticipants.cend(); ++it)
        visible.insert(it.key(), it.value());
    QList<int> connections = m_subscriberParticipants.keys();
    std::sort(connections.begin(), connections.end());
    for (int id : connections) {
        const auto &rows = m_subscriberParticipants[id];
        for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
            if (visible.size() >= kMaxParticipants && !visible.contains(it.key()))
                continue;
            const QVariantMap current = visible.value(it.key());
            const QString ownFocus = m_rtc
                ? m_rtc->participantForIdentity(m_roomId, it.key())
                    .value(QStringLiteral("focusServiceUrl")).toString()
                : QString();
            const bool preferred = !ownFocus.isEmpty()
                && RtcController::sameFocusUrl(m_connectionFocusUrls.value(id), ownFocus);
            const bool currentHasTracks = !current.value(QStringLiteral("tracks"))
                .toList().isEmpty();
            const bool candidateHasTracks = !it.value()
                .value(QStringLiteral("tracks")).toList().isEmpty();
            if (current.isEmpty() || (candidateHasTracks && !currentHasTracks)
                || (candidateHasTracks && currentHasTracks && preferred))
                visible.insert(it.key(), it.value());
        }
    }
    QVariantList merged;
    QSet<QString> added;
    for (const QVariant &previous : std::as_const(m_participants)) {
        const QString identity = previous.toMap()
            .value(QStringLiteral("identity")).toString();
        if (visible.contains(identity) && !added.contains(identity)) {
            merged.append(visible.value(identity));
            added.insert(identity);
        }
    }
    for (auto it = visible.cbegin(); it != visible.cend(); ++it) {
        if (!added.contains(it.key()))
            merged.append(it.value());
    }
    m_participants = merged;
    noteParticipantIdentities();
    rebuildModels();
    Q_EMIT participantsChanged();

    QSet<QString> after;
    for (const QVariant &row : std::as_const(m_participants)) {
        after.insert(
            row.toMap().value(QStringLiteral("identity")).toString());
    }
    return after != before;
}

void SfuCallController::onSfuSpeakers(const QVariantList &speakers)
{
    if (!sfuSessionLive())
        return;
    mergeSpeakers(speakers);
}

void SfuCallController::mergeSpeakers(const QVariantList &speakers)
{
    for (const QVariant &value : speakers) {
        const QVariantMap entry = value.toMap();
        const QString sid = entry.value(QStringLiteral("sid")).toString();
        if (sid.isEmpty())
            continue;
        if (!entry.value(QStringLiteral("active")).toBool()) {
            m_speaking.remove(sid);
            m_speakingLevel.remove(sid);
            continue;
        }
        m_speaking.insert(sid, true);
        if (entry.contains(QStringLiteral("level"))) {
            m_speakingLevel.insert(
                sid, entry.value(QStringLiteral("level")).toDouble());
        } else {
            m_speakingLevel.remove(sid);
        }
    }
    if (m_participantModel)
        m_participantModel->applySpeakers(m_speaking, m_speakingLevel);
}

void SfuCallController::onSfuConnectionQuality(const QVariantList &updates)
{
    if (!sfuSessionLive())
        return;
    QHash<QString, QString> quality;
    for (const QVariant &value : updates) {
        const QVariantMap entry = value.toMap();
        const QString sid = entry.value(QStringLiteral("sid")).toString();
        const QString level =
            entry.value(QStringLiteral("quality")).toString();
        if (sid.isEmpty() || level.isEmpty()
            || level == QLatin1String("unknown")) {
            continue;
        }
        quality.insert(sid, level);
    }
    if (quality.isEmpty())
        return;
    for (auto it = quality.cbegin(); it != quality.cend(); ++it)
        m_connectionQuality.insert(it.key(), it.value());
    if (m_participantModel)
        m_participantModel->applyConnectionQuality(quality);
}

void SfuCallController::onSfuRemoteDescription(const QString &kind,
                                                const QString &target,
                                                const QString &sdp)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!sfuSessionLive() || m_engine.isNull())
        return;
    m_engine->applyRemoteDescription(
        target == QLatin1String("publisher")
            ? SfuMediaEngine::Target::Publisher
            : SfuMediaEngine::Target::Subscriber,
        kind, sdp);
    if (m_state == State::Connecting)
        setState(State::Connected);
#else
    Q_UNUSED(kind); Q_UNUSED(target); Q_UNUSED(sdp);
#endif
}

void SfuCallController::onSfuRemoteCandidate(const QString &target,
                                              const QString &candidateInit)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!sfuSessionLive() || m_engine.isNull())
        return;
    m_engine->applyRemoteCandidate(
        target == QLatin1String("publisher")
            ? SfuMediaEngine::Target::Publisher
            : SfuMediaEngine::Target::Subscriber,
        candidateInit);
#else
    Q_UNUSED(target); Q_UNUSED(candidateInit);
#endif
}

// Never log the SDP (it carries host IPs); only that one was produced and in
// which direction, which is the first question when LiveKit times out a join.
void SfuCallController::onEngineLocalDescription(int target,
                                                  const QString &kind,
                                                  const QString &sdp)
{
    qCInfo(lcSfuCall) << "local description kind=" << kind
                      << "target=" << target
                      << "bytes=" << sdp.size()
                      << "active=" << active();
    if (!active() || !m_client)
        return;
    m_client->sfuLocalDescriptionFor(
        m_sfuGeneration, 0, kind,
        target == 0 ? QStringLiteral("publisher")
                    : QStringLiteral("subscriber"),
        sdp);
}

void SfuCallController::onEngineLocalCandidate(int target,
                                                const QString &candidateInit)
{
    ++m_candidatesSent;
    if (m_candidatesSent <= 3 || m_candidatesSent % 10 == 0) {
        qCInfo(lcSfuCall) << "local candidate #" << m_candidatesSent
                          << "target=" << target
                          << "active=" << active();
    }
    if (!active() || !m_client)
        return;
    m_client->sfuLocalCandidateFor(
        m_sfuGeneration, 0,
        target == 0 ? QStringLiteral("publisher")
                    : QStringLiteral("subscriber"),
        candidateInit);
}

bool SfuCallController::categoryIsShareAudioOnly(const QString &category)
{
    return category == QLatin1String("share_audio_failed")
        || category == QLatin1String("share_audio_unavailable")
        || category == QLatin1String("share_audio_apps_unavailable");
}

void SfuCallController::onEngineFailed(const QString &category)
{
    qCWarning(lcSfuCall) << "engine failed category=" << category
                         << "active=" << active();
    if (!active())
        return;
    if (categoryIsShareAudioOnly(category)) {
        if (!m_shareAudioCid.isEmpty()) {
            unpublishTrack(m_shareAudioCid);
            m_shareAudioCid.clear();
        }
        resetShareAudioLevel();
        Q_EMIT mediaStateChanged();
        Q_EMIT callFailed(userFacingError(category));
        return;
    }
    teardown(State::Failed, userFacingError(category));
    Q_EMIT callFailed(m_lastError);
}

void SfuCallController::onRemotePlaybackFailed(bool failed)
{
    if (!active())
        return;
    if (failed == m_playbackLostAnnounced)
        return;
    m_playbackLostAnnounced = failed;
    qCWarning(lcSfuCall) << "received audio output failed=" << failed;
    const QString notice = playbackLostNotice();
    if (failed)
        Q_EMIT callFailed(notice);
    else
        withdrawNotice(notice);
}

QString SfuCallController::playbackLostNotice()
{
    return tr("Call audio stopped: this computer's sound output disconnected. "
              "It resumes when an output is available again; if it does not, "
              "leave and rejoin the call.");
}

bool SfuCallController::withdrawNotice(const QString &notice)
{
    if (notice.isEmpty() || m_shownNotice != notice)
        return false;
    Q_EMIT callFailed(QString());
    return true;
}

void SfuCallController::announceCameraNotice(const QString &category,
                                             const QString &cid)
{
    const QString notice = userFacingError(category);
    Q_EMIT callFailed(notice);
    m_cameraNoticeCategory = category;
    m_cameraNoticeCid = cid;
}

void SfuCallController::onEnginePublishFailed(const QString &cid,
                                              const QString &category)
{
    qCWarning(lcSfuCall) << "publish failed category=" << category
                         << "camera=" << (cid == m_cameraCid)
                         << "screen=" << (cid == m_screenCid);
    if (!active() || cid.isEmpty())
        return;
    const bool camera = cid == m_cameraCid;
    QString noticeCategory = category;
    if (camera) {
        const bool viaPortal = m_cameraViaPortal;
        const bool retryingDevice = m_cameraFellBack && !viaPortal;
        m_cameraOn = false;
        unpublishTrack(m_cameraCid);
#ifdef HAVE_LIGHTNING_WEBRTC
        clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
#endif
        if (retryingDevice && category == QLatin1String("camera_busy")) {
            const qint64 remaining = m_deviceFallbackDeadlineMs
                - QDateTime::currentMSecsSinceEpoch();
            if (remaining > 0) {
                ++m_deviceFallbackBusyRetries;
                const int delay = int(std::min<qint64>(
                    remaining, std::min(2000, 250 << std::min(3, m_deviceFallbackBusyRetries - 1))));
                qCWarning(lcSfuCall) << "direct camera busy; retry="
                                      << m_deviceFallbackBusyRetries
                                      << "delayMs=" << delay;
                m_cameraOn = true;
                const quint64 token = m_deviceFallbackToken;
                QTimer::singleShot(delay, this, [this, token] {
                    publishDeviceFallback(token);
                });
                return;
            }
            qCWarning(lcSfuCall) << "direct camera still busy after retries="
                                  << m_deviceFallbackBusyRetries
                                  << "outcome=camera_busy";
        }
        const bool hopped = m_cameraFellBack || m_cameraFellBackToPortal;
        if (cameraFailureStep(category, viaPortal, cameraDeviceNodeVisible(),
                              hopped)
            == CameraFailureStep::OpenDeviceNode) {
            fallBackToDeviceCamera(category,
                (m_engine.isNull() && !m_holdCameraPortalReleaseForTest)
                    ? QString() : cid);
            return;
        }
        if (!cameraChosen()
            && directCameraFallsBackToPortal(category, viaPortal,
                                             !m_cameraPortal.isNull(),
                                             hopped)) {
            fallBackToPortalCamera(category);
            return;
        }
        noticeCategory = cameraNoticeCategory(category, viaPortal,
                                              m_cameraPortalReportedCamera);
    } else if (cid == m_screenCid) {
        m_screenSharing = false;
        unpublishTrack(m_screenCid);
        if (!m_shareAudioCid.isEmpty())
            unpublishTrack(m_shareAudioCid);
        shareAudioShareEnded();
        if (m_portal)
            m_portal->cancel();
#ifdef HAVE_LIGHTNING_WEBRTC
        clearLocalVideoSurface(SfuMediaEngine::localScreenStreamId());
#endif
    } else if (cid == m_audioCid) {
        Q_EMIT callFailed(userFacingError(category));
        return;
    } else {
        return;
    }
    applyVideoState();
    Q_EMIT mediaStateChanged();
    if (camera) {
        announceCameraNotice(noticeCategory, cid);
        return;
    }
    Q_EMIT callFailed(userFacingError(category));
}

void SfuCallController::expireParkedKeys()
{
    if (m_parkedKeys.isEmpty() || !m_parkClock.isValid())
        return;
    const qint64 now = m_parkClock.elapsed();
    const int before = m_parkedKeys.size();
    m_parkedKeys.removeIf([now](const ParkedKey &k) {
        return now - k.arrivedMs > kParkedKeyTtlMs;
    });
    if (m_parkedKeys.size() != before) {
        qCInfo(lcSfuCall) << "parked media keys expired count="
                          << (before - m_parkedKeys.size());
    }
}

void SfuCallController::parkMediaKey(const QString &roomId,
                                     const QString &sender,
                                     const QString &deviceId, int index,
                                     const QString &keyBase64)
{
    if (!m_parkClock.isValid())
        m_parkClock.start();
    expireParkedKeys();
    const qint64 now = m_parkClock.elapsed();

    for (ParkedKey &k : m_parkedKeys) {
        if (k.sender == sender && k.deviceId == deviceId && k.index == index) {
            k.roomId = roomId;
            k.keyBase64 = keyBase64;
            k.arrivedMs = now;
            return;
        }
    }
    const auto countFor = [this](const QString &s, const QString &d) {
        int n = 0;
        for (const ParkedKey &k : m_parkedKeys) {
            if (k.sender == s && k.deviceId == d)
                ++n;
        }
        return n;
    };
    while (countFor(sender, deviceId) >= kMaxParkedKeysPerDevice) {
        for (int i = 0; i < m_parkedKeys.size(); ++i) {
            if (m_parkedKeys.at(i).sender == sender
                && m_parkedKeys.at(i).deviceId == deviceId) {
                m_parkedKeys.removeAt(i);
                break;
            }
        }
    }
    while (m_parkedKeys.size() >= kMaxParkedKeys) {
        int victim = 0;
        int worst = -1;
        for (int i = 0; i < m_parkedKeys.size(); ++i) {
            const int n = countFor(m_parkedKeys.at(i).sender,
                                   m_parkedKeys.at(i).deviceId);
            if (n > worst) {
                worst = n;
                victim = i;
            }
        }
        m_parkedKeys.removeAt(victim);
    }
    m_parkedKeys.append(
        ParkedKey{roomId, sender, deviceId, index, keyBase64, now});
}

void SfuCallController::applyParkedKeys()
{
    expireParkedKeys();
    if (m_parkedKeys.isEmpty())
        return;
    const QList<ParkedKey> pending = m_parkedKeys;
    m_parkedKeys.clear();
    qCInfo(lcSfuCall) << "replaying media keys that arrived before the call"
                      << "was active count=" << pending.size();
    for (const ParkedKey &k : pending) {
        onMediaKeyReceived(k.roomId.isEmpty() ? m_roomId : k.roomId, k.sender,
                           k.deviceId, k.index, k.keyBase64);
    }
}

void SfuCallController::onMediaKeyReceived(const QString &roomId,
                                            const QString &sender,
                                            const QString &claimedDeviceId,
                                            int keyIndex,
                                            const QString &keyBase64)
{
    qCInfo(lcSfuCall) << "media key received index=" << keyIndex
                      << "forThisRoom=" << (roomId == m_roomId)
                      << "active=" << active();
    if (keyIndex < 0 || keyIndex >= CallFrameCryptor::kKeyRingSize)
        return;
    if (keyBase64.size() > 256)
        return;
    const QByteArray raw =
        QByteArray::fromBase64(keyBase64.toUtf8(),
                               QByteArray::AbortOnBase64DecodingErrors);
    if (raw.size() != 16 && raw.size() != 32) {
        qCWarning(lcSfuCall) << "media key refused: unsupported length"
                             << raw.size();
        return;
    }

    if (!active() || roomId != m_roomId) {
        parkMediaKey(roomId, sender, claimedDeviceId, keyIndex, keyBase64);
        return;
    }
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine)
        return;
    // Keys live only in the engine's cryptor (never stored, logged or in
    // QML), under the sending device's name, which is always known here. The
    // LiveKit sid is bound to it later (noteParticipantIdentities), since key,
    // participant list and membership arrive in any order.
    const QString ringName = mediaKeyRingName(sender, claimedDeviceId);
    if (!m_inboundRingKeys.contains(ringName)
        && m_inboundRingKeys.size() >= kMaxParticipants)
        return;
    m_inboundRingKeys[ringName].insert(keyIndex, raw);
    m_engine->setInboundKey(ringName, keyIndex, raw);
    for (const auto &receiver : std::as_const(m_subscriberEngines)) {
        if (!receiver.isNull())
            receiver->setInboundKey(ringName, keyIndex, raw);
    }
    m_engine->noteParticipantIdentity(
        sender + QLatin1Char(':') + claimedDeviceId, ringName);
    for (const auto &receiver : std::as_const(m_subscriberEngines)) {
        if (!receiver.isNull())
            receiver->noteParticipantIdentity(
                sender + QLatin1Char(':') + claimedDeviceId, ringName);
    }
    if (m_rtc) {
        const QString identity =
            m_rtc->rtcIdentityFor(m_roomId, sender, claimedDeviceId);
        if (!identity.isEmpty())
            m_engine->noteParticipantIdentity(identity, ringName);
        if (!identity.isEmpty()) {
            for (const auto &receiver : std::as_const(m_subscriberEngines)) {
                if (!receiver.isNull())
                    receiver->noteParticipantIdentity(identity, ringName);
            }
        }
    }
    noteParticipantIdentities();
#else
    Q_UNUSED(sender);
    Q_UNUSED(claimedDeviceId);
    Q_UNUSED(raw);
#endif
}

void SfuCallController::attachVideoSink(const QString &identity,
                                        QObject *videoSink)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    const QString cameraKey = trackKeyForSource(
        identity, QStringLiteral("camera"));
    const QString streamId = streamIdForIdentity(identity);
    if (!cameraKey.isEmpty())
        m_videoRouter->attachSink(cameraKey, sink);
    if (!streamId.isEmpty())
        m_videoRouter->attachSink(streamId, sink);
#else
    Q_UNUSED(identity); Q_UNUSED(videoSink);
#endif
}

void SfuCallController::attachScreenSink(const QString &identity,
                                         QObject *videoSink)
{
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    if (m_demoCall) {
        if (auto *sink = qobject_cast<QVideoSink *>(videoSink)) {
            const QImage still(QStringLiteral(
                ":/qt/qml/MatrixClient/resources/screenshot-demo/share-poster.jpg"));
            if (!still.isNull())
                sink->setVideoFrame(QVideoFrame(still));
        }
        return;
    }
#endif
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    const QString key = trackKeyForSource(identity,
                                          QStringLiteral("screen_share"));
    if (key.isEmpty())
        return;
    m_videoRouter->attachSink(key, sink);
#else
    Q_UNUSED(identity); Q_UNUSED(videoSink);
#endif
}

void SfuCallController::attachLocalCameraSink(QObject *videoSink)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    m_videoRouter->attachSink(SfuMediaEngine::localCameraStreamId(),
                              qobject_cast<QVideoSink *>(videoSink));
#else
    Q_UNUSED(videoSink);
#endif
}

void SfuCallController::attachLocalScreenSink(QObject *videoSink)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    m_videoRouter->attachSink(SfuMediaEngine::localScreenStreamId(),
                              qobject_cast<QVideoSink *>(videoSink));
#else
    Q_UNUSED(videoSink);
#endif
}

void SfuCallController::detachSink(QObject *videoSink)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    if (!sink)
        return;
    m_videoRouter->releaseSink(sink);
#else
    Q_UNUSED(videoSink);
#endif
}

bool SfuCallController::isRoutingVideoTo(const QString &streamId) const
{
#ifdef HAVE_LIGHTNING_WEBRTC
    return m_videoRouter && m_videoRouter->watching(streamId);
#else
    Q_UNUSED(streamId);
    return false;
#endif
}

QString SfuCallController::trackKeyForSource(const QString &identity,
                                            const QString &source) const
{
    if (identity.isEmpty() || source.isEmpty())
        return {};
    QString fallback;
    for (const QVariant &row : m_participants) {
        const QVariantMap participant = row.toMap();
        if (participant.value(QStringLiteral("identity")).toString()
            != identity) {
            continue;
        }
        for (const QVariant &t :
             participant.value(QStringLiteral("tracks")).toList()) {
            const QVariantMap track = t.toMap();
            if (track.value(QStringLiteral("source")).toString() != source)
                continue;
            const QString sid = track.value(QStringLiteral("sid")).toString();
            if (sid.isEmpty())
                continue;
            if (!track.value(QStringLiteral("muted")).toBool())
                return sid;
            if (fallback.isEmpty())
                fallback = sid;
        }
        return fallback;
    }
    return {};
}

bool SfuCallController::mediaBlockedFor(const QString &identity) const
{
    if (identity.isEmpty() || m_blockedStreams.isEmpty())
        return false;
    const QString streamId = streamIdForIdentity(identity);
    return !streamId.isEmpty() && m_blockedStreams.contains(streamId);
}

QString SfuCallController::streamIdForIdentity(const QString &identity) const
{
    if (identity.isEmpty())
        return {};
    for (const QVariant &row : m_participants) {
        const QVariantMap participant = row.toMap();
        if (participant.value(QStringLiteral("identity")).toString()
            != identity) {
            continue;
        }
        return participant.value(QStringLiteral("sid")).toString();
    }
    return {};
}

QList<SfuCallController::KeyTarget> SfuCallController::mediaKeyTargets() const
{
    QList<KeyTarget> out;
    if (!m_rtc || m_roomId.isEmpty())
        return out;
    QSet<QString> seen;
    for (const QVariant &row : std::as_const(m_participants)) {
        const QVariantMap participant = row.toMap();
        const QString identity =
            participant.value(QStringLiteral("identity")).toString();
        if (identity.isEmpty() || identity == m_ownIdentity)
            continue;
        const QVariantMap person =
            m_rtc->participantForIdentity(m_roomId, identity);
        if (person.value(QStringLiteral("ownDevice")).toBool())
            continue;
        KeyTarget target;
        target.userId = person.value(QStringLiteral("userId")).toString();
        target.deviceId = person.value(QStringLiteral("deviceId")).toString();
        target.session = participant.value(QStringLiteral("sid")).toString();
        if (target.userId.isEmpty() || target.deviceId.isEmpty())
            continue;
        const QString device = keyTargetDevice(target);
        if (seen.contains(device))
            continue;
        seen.insert(device);
        out.append(target);
    }
    return out;
}

QString SfuCallController::mediaKeyTargetsJson(const QList<KeyTarget> &targets)
{
    QJsonArray out;
    for (const KeyTarget &t : targets) {
        QJsonObject target;
        target.insert(QStringLiteral("user_id"), t.userId);
        target.insert(QStringLiteral("device_id"), t.deviceId);
        out.append(target);
    }
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}

QString SfuCallController::keyTargetDevice(const KeyTarget &target)
{
    return target.userId + QChar(0x1f) + target.deviceId;
}

QString SfuCallController::mediaKeyRingName(const QString &userId,
                                            const QString &deviceId)
{
    if (userId.isEmpty() || deviceId.isEmpty())
        return {};
    return userId + QChar(0x1f) + deviceId;
}

void SfuCallController::noteParticipantIdentities()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_rtc)
        return;
    QVariantList allRows;
    for (const QVariantMap &row : std::as_const(m_publisherParticipants))
        allRows.append(row);
    for (auto it = m_subscriberParticipants.cbegin();
         it != m_subscriberParticipants.cend(); ++it) {
        for (const QVariantMap &row : it.value())
            allRows.append(row);
    }
    for (const QVariant &row : std::as_const(allRows)) {
        const QVariantMap participant = row.toMap();
        const QString identity =
            participant.value(QStringLiteral("identity")).toString();
        const QString sid = participant.value(QStringLiteral("sid")).toString();
        if (identity.isEmpty() || sid.isEmpty())
            continue;
        SfuMediaEngine *receiver = m_engine.data();
        QString rawSid = sid;
        for (auto it = m_subscriberEngines.cbegin();
             it != m_subscriberEngines.cend(); ++it) {
            const QString prefix = QStringLiteral("c%1:").arg(it.key());
            if (sid.startsWith(prefix)) {
                receiver = it.value().data();
                rawSid = sid.mid(prefix.size());
                break;
            }
        }
        if (!receiver)
            continue;
        receiver->noteParticipantIdentity(rawSid, identity);
        const QVariantMap person =
            m_rtc->participantForIdentity(m_roomId, identity);
        const QString name = mediaKeyRingName(
            person.value(QStringLiteral("userId")).toString(),
            person.value(QStringLiteral("deviceId")).toString());
        if (!name.isEmpty()) {
            receiver->noteParticipantIdentity(rawSid, name);
            continue;
        }
        m_rtc->refreshFromServer(m_roomId);
    }
#endif
}

void SfuCallController::reconcileKeyLane()
{
    if (!active())
        return;
    ++m_keyLaneReconciles;
    noteParticipantIdentities();
    distributeKeyIfNeeded();
}

int SfuCallController::sfuPeerCount() const
{
    int peers = 0;
    for (const QVariant &row : std::as_const(m_participants)) {
        const QString identity =
            row.toMap().value(QStringLiteral("identity")).toString();
        if (!identity.isEmpty() && identity != m_ownIdentity)
            ++peers;
    }
    return peers;
}

bool SfuCallController::keyLaneReady() const
{
    if (!active() || !m_client || !m_rtc || !m_roomEncrypted)
        return false;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull())
        return false;
#endif
    return true;
}

void SfuCallController::startKeyLane()
{
    if (!keyLaneReady())
        return;
    if (m_newestKey.serial != 0) {
        distributeKeyIfNeeded();
        return;
    }
    rotateMediaKey(mediaKeyTargets(), "first key");
}

void SfuCallController::distributeKeyIfNeeded()
{
    if (!keyLaneReady())
        return;
    if (m_newestKey.serial == 0)
        return;
    const QList<KeyTarget> targets = mediaKeyTargets();
    QSet<QString> present;
    for (const KeyTarget &t : targets)
        present.insert(keyTargetDevice(t));

    if (targets.isEmpty()) {
        const int sfuPeers = sfuPeerCount();
        const bool keyWithheld = m_deliveredKeyIndex >= 0
            && m_newestKey.index != m_deliveredKeyIndex;
        const int logged = sfuPeers * 2 + (keyWithheld ? 1 : 0);
        if (logged != m_unaddressableLogged) {
            m_unaddressableLogged = logged;
            qCInfo(lcSfuCall) << "media key: nobody addressable"
                              << "sfuPeers=" << sfuPeers
                              << "keyWithheld=" << keyWithheld;
        }
        if (sfuPeers > 0 || keyWithheld)
            m_rtc->refreshFromServer(m_roomId);
    } else {
        m_unaddressableLogged = -1;
    }

    int left = 0;
    for (auto it = m_keyRecipients.cbegin(); it != m_keyRecipients.cend();
         ++it) {
        if (!present.contains(it.key()))
            ++left;
    }
    int joined = 0;
    QList<KeyTarget> needKey;
    for (const KeyTarget &t : targets) {
        const QString device = keyTargetDevice(t);
        const auto r = m_keyRecipients.constFind(device);
        const bool isNew = r == m_keyRecipients.cend() || r.value() != t.session;
        if (isNew) {
            ++joined;
            if (r != m_keyRecipients.cend())
                m_keyResendBackoff.remove(device);
            needKey.append(t);
            continue;
        }
        if (m_keyHolders.contains(device))
            continue;
        const auto backoff = m_keyResendBackoff.constFind(device);
        if (backoff != m_keyResendBackoff.cend()
            && m_keyClock.elapsed() < backoff->notBeforeMs) {
            ++m_keyResendsHeldBack;
            continue;
        }
        needKey.append(t);
    }

    const bool switchPending = keySwitchPending();
    if (left > 0 && !switchPending) {
        rotateMediaKey(targets, "a holder left");
        return;
    }
    if (left > 0) {
        qCInfo(lcSfuCall) << "media key: a holder left during a pending"
                          << "switch; rotating after it left=" << left;
    }
    const qint64 keyAgeMs = m_keyClock.elapsed() - m_newestKey.mintedMs;
    if (joined > 0 && !switchPending && keyAgeMs >= m_joinKeyGraceMs) {
        rotateMediaKey(targets, "a joiner; the key is older than the grace");
        return;
    }
    if (!needKey.isEmpty()) {
        qCInfo(lcSfuCall) << "media key shared index=" << m_newestKey.index
                          << "targets=" << needKey.size()
                          << "joined=" << joined << "(no rotation)";
        sendNewestKeyTo(needKey);
        return;
    }
    if (!targets.isEmpty())
        qCDebug(lcSfuCall) << "media key: no redistribution, set unchanged";
}

void SfuCallController::rotateMediaKey(const QList<KeyTarget> &targets,
                                       const char *reason)
{
    const bool first = (m_adoptedKeySerial == 0);
    const bool switchNow = first || !someonePresentHoldsAdoptedKey(targets);

    // kMintedKeyBytes from the system CSPRNG; never the generic PRNG for keys.
    // A multiple of 4, as generate() fills whole quint32s.
    static_assert(kMintedKeyBytes % 4 == 0);
    QByteArray key(kMintedKeyBytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32 *>(key.data()),
        reinterpret_cast<quint32 *>(key.data() + key.size()));
    m_keyCursor = (m_keyCursor + 1) % kSendKeyIndices;
    if (m_adoptedKeySerial != 0 && m_keyCursor == m_adoptedKeyIndex)
        m_keyCursor = (m_keyCursor + 1) % kSendKeyIndices;

    m_useKeyTimer.stop();
    m_keySwitchFallbackTimer.stop();
    m_newestKey.raw.fill('\0');
    m_newestKey.index = m_keyCursor;
    m_newestKey.raw = std::move(key);
    m_newestKey.mintedMs = m_keyClock.elapsed();
    m_newestKey.serial = ++m_keySerialCounter;
    m_keyRecipients.clear();
    m_keyHolders.clear();

    const int sfuPeers = sfuPeerCount();
    qCInfo(lcSfuCall) << "media key distributed index=" << m_newestKey.index
                      << "targets=" << targets.size()
                      << "sfuPeers=" << sfuPeers
                      << "unresolved=" << (sfuPeers - targets.size())
                      << "reason=" << reason;
    {
        QStringList named;
        named.reserve(targets.size());
        for (const KeyTarget &t : targets)
            named << (t.userId + QLatin1Char('/') + t.deviceId);
        qCInfo(lcSfuCall) << "media key targeted devices="
                          << (named.isEmpty() ? QStringLiteral("<none>")
                                              : named.join(QLatin1String(", ")));
    }

    sendNewestKeyTo(targets);
    if (switchNow) {
        installNewestKey();
        return;
    }
    const int fallbackMs = m_keySwitchFallbackTimer.isActive()
        ? m_keySwitchFallbackTimer.interval()
        : -1;
    qCInfo(lcSfuCall) << "media key switch pending index=" << m_newestKey.index
                      << "afterAnswerMs=" << m_useKeyTimer.interval()
                      << "fallbackMs=" << fallbackMs
                      << "stillUsing=" << m_adoptedKeyIndex;
}

void SfuCallController::sendNewestKeyTo(const QList<KeyTarget> &targets)
{
    if (targets.isEmpty() || m_newestKey.serial == 0 || !m_client)
        return;
    const quint64 op = m_client->rtcSendMediaKey(
        m_roomId, QString::fromLatin1(m_newestKey.raw.toBase64()),
        m_newestKey.index, mediaKeyTargetsJson(targets));
    if (op == 0) {
        qCWarning(lcSfuCall) << "media key send was not dispatched index="
                             << m_newestKey.index;
        return;
    }
    const bool inUse = m_newestKey.serial == m_adoptedKeySerial;
    KeySend send;
    send.serial = m_newestKey.serial;
    for (const KeyTarget &t : targets) {
        const QString device = keyTargetDevice(t);
        send.devices.append(device);
        if (m_adoptedKeyRecipients.contains(device))
            send.keyInUseHolders.append(device);
        m_keyRecipients.insert(device, t.session);
        m_keyHolders.insert(device, t.session);
        if (inUse)
            m_adoptedKeyRecipients.insert(device);
    }
    m_keySendOps.insert(op, send);
    if (!inUse && !keySwitchPending())
        m_keySwitchFallbackTimer.start();
}

void SfuCallController::onMediaKeySent(quint64 op, bool ok,
                                       const QString &category, int delivered,
                                       int keyIndex,
                                       const QVariantList &undelivered)
{
    const auto it = m_keySendOps.constFind(op);
    const bool thisCall = it != m_keySendOps.cend();
    const KeySend send = thisCall ? it.value() : KeySend{};
    if (thisCall)
        m_keySendOps.remove(op);
    QSet<QString> missed;
    for (const QVariant &row : undelivered) {
        const QVariantMap pair = row.toMap();
        KeyTarget t;
        t.userId = pair.value(QStringLiteral("user_id")).toString();
        t.deviceId = pair.value(QStringLiteral("device_id")).toString();
        if (!t.userId.isEmpty() && !t.deviceId.isEmpty())
            missed.insert(keyTargetDevice(t));
    }
    if (ok) {
        qCInfo(lcSfuCall) << "media key sent index=" << keyIndex
                          << "delivered=" << delivered
                          << "undelivered=" << missed.size()
                          << "thisCall=" << thisCall;
        if (!thisCall)
            return;
        for (const QString &device : send.devices) {
            if (missed.contains(device))
                noteKeyDeliveryFailed(device);
            else
                m_keyResendBackoff.remove(device);
        }
        if (send.serial == m_newestKey.serial) {
            for (const QString &device : std::as_const(missed)) {
                if (send.devices.contains(device))
                    m_keyHolders.remove(device);
            }
        }
        if (delivered <= 0)
            return;
        m_deliveredKeyIndex = keyIndex;
        bool reachesKeyInUse = false;
        for (const QString &device : send.keyInUseHolders) {
            if (!missed.contains(device)) {
                reachesKeyInUse = true;
                break;
            }
        }
        startUseKeyDelay(send.serial, reachesKeyInUse);
        return;
    }
    qCWarning(lcSfuCall) << "media key NOT sent index=" << keyIndex
                         << "category=" << category
                         << "delivered=" << delivered
                         << "thisCall=" << thisCall;
    if (thisCall) {
        for (const QString &device : std::as_const(missed)) {
            if (send.devices.contains(device))
                noteKeyDeliveryFailed(device);
        }
    }
    if (thisCall && send.serial == m_newestKey.serial) {
        for (const QString &device : send.devices)
            m_keyHolders.remove(device);
    }
}

void SfuCallController::startUseKeyDelay(quint64 serial, bool reachesKeyInUse)
{
    if (serial == 0 || serial != m_newestKey.serial
        || m_newestKey.serial == m_adoptedKeySerial
        || m_useKeyTimer.isActive()) {
        return;
    }
    if (!reachesKeyInUse) {
        for (const KeyTarget &t : mediaKeyTargets()) {
            if (m_adoptedKeyRecipients.contains(keyTargetDevice(t)))
                return;
        }
    }
    m_useKeyTimer.start();
    qCInfo(lcSfuCall) << "media key send answered; switching in"
                      << m_useKeyTimer.interval()
                      << "ms index=" << m_newestKey.index;
}

bool SfuCallController::keySwitchPending() const
{
    return m_useKeyTimer.isActive() || m_keySwitchFallbackTimer.isActive();
}

void SfuCallController::adoptNewestKey()
{
    if (!keyLaneReady() || m_newestKey.serial == 0
        || m_newestKey.serial == m_adoptedKeySerial) {
        return;
    }
    // Never encrypt under a key that reached nobody while somebody present
    // can still decrypt the one in use: every later frame would be
    // unreadable while looking healthy here.
    // Applies to the use-key delay and the fallback alike.
    if (m_keyHolders.isEmpty()
        && someonePresentHoldsAdoptedKey(mediaKeyTargets())) {
        qCWarning(lcSfuCall)
            << "media key NOT adopted index=" << m_newestKey.index
            << "— it reached nobody; still encrypting under index="
            << m_adoptedKeyIndex
            << "which a peer holds. It is sent again when someone is"
            << "addressable.";
        m_useKeyTimer.stop();
        m_keySwitchFallbackTimer.stop();
        return;
    }
    installNewestKey();
    qCInfo(lcSfuCall) << "media key switched to index=" << m_newestKey.index;
    distributeKeyIfNeeded();
}

void SfuCallController::installNewestKey()
{
    m_useKeyTimer.stop();
    m_keySwitchFallbackTimer.stop();
    m_adoptedKeySerial = m_newestKey.serial;
    m_adoptedKeyIndex = m_newestKey.index;
    m_adoptedKeyRecipients.clear();
    for (auto it = m_keyRecipients.cbegin(); it != m_keyRecipients.cend(); ++it)
        m_adoptedKeyRecipients.insert(it.key());
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull())
        return;
    m_engine->setOutboundKey(m_newestKey.index, m_newestKey.raw, true);
    const bool encrypted = m_engine->encryptionActive();
    if (encrypted != m_mediaEncrypted) {
        m_mediaEncrypted = encrypted;
        Q_EMIT mediaStateChanged();
    }
#endif
}

bool SfuCallController::someonePresentHoldsAdoptedKey(
    const QList<KeyTarget> &targets) const
{
    if (m_adoptedKeyRecipients.isEmpty())
        return false;
    for (const KeyTarget &t : targets) {
        if (m_adoptedKeyRecipients.contains(keyTargetDevice(t)))
            return true;
    }
    return sfuPeerCount() > targets.size();
}

void SfuCallController::refreshMembership()
{
    if (!active() || !m_client || m_roomId.isEmpty())
        return;
    if (!m_delayId.isEmpty()) {
        m_delayedRestartOp = m_client->rtcRestartDelayedLeave(m_delayId);
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = now - m_lastPublishMs;
    if (m_lastPublishMs != 0 && elapsed >= 0
        && elapsed < kMembershipRepublishIntervalMs) {
        return;
    }
    republishMembership();
}

void SfuCallController::republishMembership()
{
    if (!active() || !m_client || m_roomId.isEmpty())
        return;
    m_lastPublishMs = QDateTime::currentMSecsSinceEpoch();
    m_refreshOp = m_client->rtcPublishMembership(
        m_roomId, m_focusUrl,
        m_withVideo ? QStringLiteral("video") : QStringLiteral("audio"),
        m_rtc && !m_rtc->ownFocusUrlFor().isEmpty()
            && RtcController::sameFocusUrl(m_focusUrl, m_rtc->ownFocusUrlFor())
            && (!m_rtc->usesLegacyFocusFor(m_roomId) || m_publishFallbackTried));
    if (m_refreshOp == 0) {
        qCWarning(lcSfuCall)
            << "membership refresh could not be dispatched — this device may "
               "expire out of the call while still connected";
    }
}

void SfuCallController::abandonOutstandingMembershipWrites()
{
    // Op ids come from the client's own counter, and the app's one client
    // serves every account in turn: a retry still armed after a sign-out or
    // an account switch would dispatch the old account's retraction under
    // the new account's session, and a stale op id could match an unrelated
    // op of the new one. Warn only if a retry was already armed (an attempt
    // had failed); a retraction just dispatched will probably land.
    if (m_retractAttempts > 1 || m_retractRetryTimer.isActive()) {
        qCWarning(lcSfuCall)
            << "a FAILED call retraction is being abandoned because the "
               "account changed; that membership is now the server's to "
               "expire";
    }
    const bool wasPending = membershipWritesPending();
    clearOwedRetraction();
    m_supersededRetractOp = 0;
    m_supersededRetractRoomId.clear();
    m_abandonedPublishOp = 0;
    m_abandonedPublishRoomId.clear();
    if (wasPending)
        Q_EMIT membershipWritesSettled();
}

bool SfuCallController::membershipWritesPending() const
{
    return m_retractOp != 0 || !m_retractRoomId.isEmpty()
        || m_abandonedPublishOp != 0;
}

void SfuCallController::onMembershipRetracted(quint64 opId, bool ok,
                                              const QString &category)
{
    const MembershipSettleGuard settle(this);
    if (opId == 0)
        return;
    if (m_delayedRestartOp != 0 && opId == m_delayedRestartOp) {
        m_delayedRestartOp = 0;
        if (ok)
            return;
        qCWarning(lcSfuCall)
            << "delayed leave restart FAILED category=" << category;
        m_delayId.clear();
        republishMembership();
        return;
    }
    if (m_supersededRetractOp != 0 && opId == m_supersededRetractOp) {
        const QString room = m_supersededRetractRoomId;
        m_supersededRetractOp = 0;
        m_supersededRetractRoomId.clear();
        if (!ok || !active() || room != m_roomId)
            return;
        if (!m_delayId.isEmpty() || m_publishOp != 0 || m_refreshOp != 0) {
            qCWarning(lcSfuCall)
                << "a retraction from before the rejoin landed in this "
                   "room's call; not re-publishing now (delayed="
                << !m_delayId.isEmpty() << "publish_in_flight="
                << (m_publishOp != 0 || m_refreshOp != 0) << ")";
            return;
        }
        qCWarning(lcSfuCall)
            << "a retraction from before the rejoin landed in this room's "
               "call; re-publishing our membership";
        republishMembership();
        return;
    }
    if (m_retractOp == 0 || opId != m_retractOp)
        return;
    m_retractOp = 0;
    m_retractAnswerTimer.stop();
    if (ok) {
        qCInfo(lcSfuCall) << "membership retracted attempts="
                          << m_retractAttempts;
        clearOwedRetraction();
        return;
    }
    qCWarning(lcSfuCall) << "membership retraction FAILED category="
                         << category << "attempt=" << m_retractAttempts;
    const bool transient = category == QLatin1String("network")
        || category == QLatin1String("rate_limited");
    if (!transient || m_retractRoomId.isEmpty()) {
        qCWarning(lcSfuCall)
            << "giving up on the retraction. This device stays in the room's "
               "call membership until the server's delayed retraction fires, "
               "or until the membership expires. Nothing further is sent.";
        clearOwedRetraction();
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 left = m_retractDeadlineMs - now;
    if (left <= 0) {
        qCWarning(lcSfuCall)
            << "giving up on the retraction: the membership has expired for "
               "every reader by now. Nothing further is sent.";
        clearOwedRetraction();
        return;
    }
    qint64 delay = kRetractRetryDelayMs;
    if (m_retractRetryPromptly) {
        m_retractRetryPromptly = false;
    } else {
        const int doublings = qBound(0, m_retractAttempts - 1, 10);
        delay = qMin<qint64>(qint64(kRetractRetryDelayMs) << doublings,
                             kRetractRetryMaxDelayMs);
    }
    delay = qMin(delay, left);
    qCInfo(lcSfuCall) << "call retraction still owed; next attempt in ms="
                      << delay << "or as soon as the homeserver answers";
    m_retractRetryTimer.start(static_cast<int>(delay));
}

void SfuCallController::onHomeserverReachable()
{
    const MembershipSettleGuard settle(this);
    if (m_retractRoomId.isEmpty())
        return;
    if (m_retractOp != 0) {
        m_retractRetryPromptly = true;
        return;
    }
    if (!owedRetractionStillDue())
        return;
    qCInfo(lcSfuCall)
        << "the homeserver is reachable again; sending the owed call "
           "retraction now";
    m_retractRetryTimer.stop();
    dispatchRetraction();
}

void SfuCallController::retryRetraction()
{
    const MembershipSettleGuard settle(this);
    if (m_retractRoomId.isEmpty() || m_retractOp != 0)
        return;
    if (!owedRetractionStillDue())
        return;
    dispatchRetraction();
}

bool SfuCallController::owedRetractionStillDue()
{
    const QString current = m_client ? m_client->currentUserId() : QString();
    const QString device = m_client ? m_client->currentDeviceId() : QString();
    if (current != m_retractUserId || device != m_retractDeviceId) {
        qCWarning(lcSfuCall)
            << "an owed call retraction belongs to an account that is no "
               "longer the client's; dropped, the membership is the server's "
               "to expire";
        clearOwedRetraction();
        return false;
    }
    if (QDateTime::currentMSecsSinceEpoch()
        > m_retractDeadlineMs + kRetractRetryDelayMs) {
        qCInfo(lcSfuCall)
            << "an owed call retraction is past the membership's expiry; "
               "nothing further is sent";
        clearOwedRetraction();
        return false;
    }
    return true;
}

void SfuCallController::startRetraction(const QString &roomId,
                                        const QString &delayId,
                                        qint64 acceptedAtMs, bool longExpiry)
{
    if (!m_client || roomId.isEmpty())
        return;
    if (!m_retractRoomId.isEmpty() && m_retractRoomId != roomId) {
        qCWarning(lcSfuCall)
            << "an owed call retraction for another room is superseded by "
               "this leave; that membership is now the server's to expire";
    }
    clearOwedRetraction();
    m_retractRoomId = roomId;
    m_retractDelayId = delayId;
    m_retractUserId = m_client->currentUserId();
    m_retractDeviceId = m_client->currentDeviceId();
    m_retractDeadlineMs = acceptedAtMs
        + (longExpiry ? m_retractOwedLongForMs : m_retractOwedForMs);
    dispatchRetraction();
}

void SfuCallController::dispatchRetraction()
{
    if (!m_client || m_retractRoomId.isEmpty())
        return;
    m_retractRetryTimer.stop();
    ++m_retractAttempts;
    m_retractOp =
        m_client->rtcRetractMembership(m_retractRoomId, m_retractDelayId);
    if (m_retractOp != 0) {
        m_retractAnswerTimer.start(m_retractAnswerTimeoutMs);
        return;
    }
    qCWarning(lcSfuCall)
        << "retraction could not be dispatched — this device will remain in "
           "the room's call membership until it expires";
    clearOwedRetraction();
}

void SfuCallController::supersedeOwedRetractionFor(const QString &roomId)
{
    if (roomId.isEmpty() || m_retractRoomId != roomId)
        return;
    if (m_retractOp != 0) {
        m_supersededRetractOp = m_retractOp;
        m_supersededRetractRoomId = roomId;
    }
    clearOwedRetraction();
}

void SfuCallController::clearOwedRetraction()
{
    m_retractRetryTimer.stop();
    m_retractAnswerTimer.stop();
    m_retractOp = 0;
    m_retractRoomId.clear();
    m_retractDelayId.clear();
    m_retractUserId.clear();
    m_retractDeviceId.clear();
    m_retractDeadlineMs = 0;
    m_retractAttempts = 0;
    m_retractRetryPromptly = false;
}

void SfuCallController::leave()
{
    teardown(State::Ended);
}

void SfuCallController::teardown(State finalState, const QString &error)
{
    qCInfo(lcSfuCall) << "teardown state=" << static_cast<int>(finalState)
                      << "error=" << (error.isEmpty()
                                      ? QStringLiteral("<none>") : error);
    ++m_generation;
    ++m_deviceFallbackToken;
    m_waitingPortalReleaseCid.clear();
    m_releasedPortalCid.clear();
    stopReconnect();
    m_shareSourceReusable = false;
    m_transportEverConnected = false;
    m_transportStates.clear();
    m_reconnectEpisodeStarts.clear();
    m_refreshTimer.stop();
    // Remember a publish still in flight: the server may apply it after our
    // retraction, recreating a membership for a device that left.
    // onMembershipPublished() retracts it when the answer lands. The join's
    // publish and a heartbeat re-publish write the same state key, so one
    // slot holding the newest op is enough.
    if (m_publishOp != 0 || m_refreshOp != 0) {
        m_abandonedPublishOp = m_publishOp != 0 ? m_publishOp : m_refreshOp;
        m_abandonedPublishRoomId = m_roomId;
    }
    m_publishOp = 0;
    m_refreshOp = 0;
    m_delayedRestartOp = 0;
    m_lastPublishMs = 0;
    m_announceOp = 0;
    m_announceEventId.clear();
    m_announcePeer.clear();
    m_pendingDeclineTarget.clear();
    resetKeyLane();
    if (m_playbackLostAnnounced)
        withdrawNotice(playbackLostNotice());
    m_playbackLostAnnounced = false;
    withdrawNotice(userFacingError(QStringLiteral("audio_source_failed")));
    if (!m_cameraNoticeCategory.isEmpty())
        withdrawNotice(userFacingError(m_cameraNoticeCategory));
    m_cameraNoticeCategory.clear();
    m_cameraNoticeCid.clear();

#ifdef HAVE_LIGHTNING_WEBRTC
    clearSubscriberEngines();
    if (!m_engine.isNull())
        m_engine->stop();
#endif
    ++m_callGeneration;
    m_sfuGeneration = 0;
    m_connectionFocusUrls.clear();
    m_inboundRingKeys.clear();
    if (m_portal)
        m_portal->cancel();
    m_cameraAwaitingPortal = false;
    if (m_cameraPortal)
        m_cameraPortal->cancel();
    if (m_client) {
        m_client->sfuDisconnect();
        if (!m_roomId.isEmpty() && m_membershipPublished) {
            startRetraction(m_roomId, m_delayId,
                            m_membershipAcceptedMs > 0
                                ? m_membershipAcceptedMs
                                : QDateTime::currentMSecsSinceEpoch(),
                            m_membershipLongExpiryUnguarded);
        }
    }

    m_roomId.clear();
    m_focusUrl.clear();
    m_membershipEventId.clear();
    m_membershipPublished = false;
    m_membershipAcceptedMs = 0;
    m_membershipLongExpiryUnguarded = false;
    m_delayId.clear();
    m_delayedCategory.clear();
    m_ownIdentity.clear();
    m_participants.clear();
    m_publisherParticipants.clear();
    m_remoteTrackMuted.clear();
    if (!m_blockedStreams.isEmpty()) {
        m_blockedStreams.clear();
        Q_EMIT remoteMediaBlockedChanged();
    }
    if (m_microphoneSilent) {
        m_microphoneSilent = false;
        Q_EMIT microphoneSilentChanged();
    }
    resetMicrophoneLevel();
    setNoiseSuppressionFailedMode(QString());
    // Key material never outlives the call.
    m_parkedKeys.clear();
    m_speaking.clear();
    m_speakingLevel.clear();
    m_connectionQuality.clear();
    if (m_participantModel)
        m_participantModel->clear();
    if (m_shareModel)
        m_shareModel->clear();
    if (m_stageState)
        m_stageState->clear();
    m_publishedTrackIds.clear();
    m_publishedTrackSids.clear();
    m_shareVolumes.clear();
    m_engineParticipantVolume.clear();
    m_engineShareVolume.clear();
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_shareAudioCid.clear();
    m_cameraOn = false;
    m_cameraFellBack = false;
    m_cameraFellBackToPortal = false;
    m_screenSharing = false;
    shareAudioShareEnded();
    m_handRaised = false;
    m_handReactionId.clear();
    m_handOp = 0;
    m_handReactions.clear();
    m_pendingAnnotations.clear();
    m_reactionOp = 0;
    m_lastReactionSentMs = 0;
    m_mediaEncrypted = false;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_videoRouter)
        m_videoRouter->clear();
#endif
    setState(finalState, error);
    Q_EMIT mediaStateChanged();
    Q_EMIT participantsChanged();
}

void SfuCallController::noteMicrophoneLevelAt(double peakDb, qint64 nowMs)
{
    if (m_micMuted || !active())
        return;
    double shown = 0.0;
    if (!m_microphoneMeter.offer(peakDb, nowMs, &shown))
        return;
    const double fraction = lightning::calls::meterFraction(shown);
    if (qFuzzyCompare(1.0 + fraction, 1.0 + m_microphoneLevel))
        return;
    m_microphoneLevel = fraction;
    Q_EMIT microphoneLevelChanged();
}

void SfuCallController::resetMicrophoneLevel()
{
    m_microphoneMeter.reset();
    if (m_microphoneLevel == 0.0)
        return;
    m_microphoneLevel = 0.0;
    Q_EMIT microphoneLevelChanged();
}

void SfuCallController::setNoiseSuppressionFailedMode(const QString &mode,
                                                     bool fallbackToWebrtc)
{
    const bool fallback = !mode.isEmpty() && fallbackToWebrtc;
    if (m_noiseSuppressionFailedMode == mode
        && m_noiseSuppressionFallbackToWebrtc == fallback)
        return;
    m_noiseSuppressionFailedMode = mode;
    m_noiseSuppressionFallbackToWebrtc = fallback;
    Q_EMIT noiseSuppressionFailedModeChanged();
}

void SfuCallController::retryNoiseSuppression()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull() && !m_noiseSuppressionFailedMode.isEmpty())
        m_engine->retryNoiseSuppression();
#endif
}

QVariantList SfuCallController::noiseSuppressionChoices() const
{
    QVariantList out;
    for (const calls::noise::Mode mode :
         {calls::noise::Mode::Off, calls::noise::Mode::WebRtc,
          calls::noise::Mode::RNNoise, calls::noise::Mode::DeepFilterNet}) {
        QString reason;
#ifdef HAVE_LIGHTNING_WEBRTC
        reason = QLatin1String(calls::noise::unavailableKey(
            calls::noise::availability(mode, !m_engine.isNull())));
#else
        if (mode != calls::noise::Mode::Off)
            reason = QStringLiteral("no-call-engine");
#endif
        out.append(QVariantMap{
            {QStringLiteral("key"),
             QLatin1String(calls::noise::modeKey(mode))},
            {QStringLiteral("available"), reason.isEmpty()},
            {QStringLiteral("reason"), reason},
        });
    }
    return out;
}

void SfuCallController::applyAudioState()
{
    if (m_micMuted)
        resetMicrophoneLevel();
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull())
        return;
    m_engine->setMicrophoneMuted(m_micMuted);
    m_engine->setOutputMuted(m_deafened);
    for (const auto &receiver : std::as_const(m_subscriberEngines)) {
        if (!receiver.isNull())
            receiver->setOutputMuted(m_deafened);
    }
    m_engine->setMicrophoneGain(m_settings ? m_settings->microphoneGain()
                                           : 100);
    m_engine->setVoiceProcessing(m_settings ? m_settings->echoCancellation()
                                            : false,
                                 m_settings ? m_settings->autoGainControl()
                                            : true);
    m_engine->setNoiseSuppressionMode(
        m_settings ? calls::noise::modeFromKey(
                         m_settings->noiseSuppressionMode().toStdString(),
                         calls::noise::kDefaultMode)
                   : calls::noise::kDefaultMode);
#endif
    syncMicMuteToSfu();
}

void SfuCallController::setMicrophoneMuted(bool muted)
{
    if (m_micMuted == muted)
        return;
    m_micMuted = muted;
    if (!muted && m_deafened)
        m_deafened = false;
    applyAudioState();
    Q_EMIT mediaStateChanged();
}

void SfuCallController::toggleMicrophoneMuted()
{
    setMicrophoneMuted(!m_micMuted);
}

void SfuCallController::setDeafened(bool deafened)
{
    if (m_deafened == deafened)
        return;
    if (deafened) {
        m_micMutedBeforeDeafen = m_micMuted;
        m_deafened = true;
        m_micMuted = true;
    } else {
        m_deafened = false;
        m_micMuted = m_micMutedBeforeDeafen;
    }
    applyAudioState();
    Q_EMIT mediaStateChanged();
}

void SfuCallController::toggleDeafened() { setDeafened(!m_deafened); }

void SfuCallController::startCameraCapture()
{
    if (!cameraEngineReady() || !m_client)
        return;

    const bool sandboxed = cameraSandboxed();
    const bool deviceNode = cameraDeviceNodeVisible();
    const bool portalWired = !m_cameraPortal.isNull();
    const bool portalUsable = cameraPortalUsable();
    LinuxCameraRoute route =
        linuxCameraRoute(sandboxed, portalUsable, deviceNode, cameraChosen());
    if (route == LinuxCameraRoute::Portal && m_cameraFellBack && deviceNode) {
        qCInfo(lcSfuCall) << "camera portal gave no picture earlier in this "
                             "call; using the device node";
        route = LinuxCameraRoute::Direct;
    }
    if (route == LinuxCameraRoute::Direct && m_cameraFellBackToPortal
        && portalWired && !cameraChosen()) {
        qCInfo(lcSfuCall) << "camera device node was busy earlier in this "
                             "call; using the portal";
        route = LinuxCameraRoute::Portal;
    }
    const bool nothingToOpen = sandboxed && !deviceNode;
    if (route == LinuxCameraRoute::Portal && !portalWired && !nothingToOpen) {
        qCWarning(lcSfuCall)
            << "camera route=portal but no camera portal is wired; falling "
               "back to the direct device";
        route = LinuxCameraRoute::Direct;
    }
    qCInfo(lcSfuCall) << "camera route="
                      << (route == LinuxCameraRoute::Portal ? "portal"
                                                            : "direct")
                      << "sandboxed=" << sandboxed
                      << "device_node=" << deviceNode
                      << "portal_wired=" << portalWired
                      << "portal_usable=" << portalUsable;

    m_cameraViaPortal = route == LinuxCameraRoute::Portal;
    m_cameraPortalReportedCamera = portalUsable;
    if (route == LinuxCameraRoute::Portal && !portalWired) {
        qCWarning(lcSfuCall) << "no camera portal and no device node in the "
                                "sandbox; no camera can be opened";
        abandonPendingCamera();
        announceCameraNotice(QStringLiteral("camera_portal_unavailable"),
                             QString());
        return;
    }
    if (route == LinuxCameraRoute::Direct) {
        publishCameraTrack(-1);
        return;
    }

    m_cameraAwaitingPortal = true;
    requestCameraAccess();
}

void SfuCallController::publishCameraTrack(int pipewireFd)
{
    if (m_cameraPublisherForTest && m_client) {
        bool engine = false;
#ifdef HAVE_LIGHTNING_WEBRTC
        engine = !m_engine.isNull();
#endif
        if (!engine) {
            m_cameraViaPortal = pipewireFd >= 0;
            ++m_cameraPublishesForTest;
            m_cameraPublisherForTest(pipewireFd);
            m_cameraCid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_publishedTrackIds.append(m_cameraCid);
            return;
        }
    }
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_client) {
        closePortalFd(pipewireFd);
        return;
    }
    const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_engine->setCameraQuality(m_settings
                                   ? m_settings->effectiveCameraResolution()
                                   : SfuMediaEngine::kCameraHeight);
    const int cameraH = m_engine->cameraMaxHeight();
    m_cameraCid = cid;
    m_publishedTrackIds.append(cid);
    m_client->sfuAddTrackFor(m_sfuGeneration, 0, cid, QStringLiteral("camera"), 1,
                          (cameraH * 16) / 9, cameraH,
                          false, m_roomEncrypted);
    m_cameraViaPortal = pipewireFd >= 0;
    m_engine->publishVideo(cid,false,-1,
                           pipewireFd);
#else
    closePortalFd(pipewireFd);
#endif
}

void SfuCallController::abandonPendingCamera()
{
    m_cameraAwaitingPortal = false;
    if (!m_cameraOn)
        return;
    m_cameraOn = false;
    applyVideoState();
    Q_EMIT mediaStateChanged();
}

void SfuCallController::setCameraOn(bool on)
{
    if (m_cameraOn == on || !active() || !cameraEngineReady() || !m_client)
        return;
    if (m_state == State::Reconnecting) {
        m_cameraOn = on;
#ifdef HAVE_LIGHTNING_WEBRTC
        if (!on)
            clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
#endif
        Q_EMIT mediaStateChanged();
        return;
    }
    m_cameraOn = on;
    if (on) {
        ++m_deviceFallbackToken;
        m_waitingPortalReleaseCid.clear();
        m_cameraFellBack = false;
        m_cameraFellBackToPortal = false;
        startCameraCapture();
    } else {
        ++m_deviceFallbackToken;
        m_waitingPortalReleaseCid.clear();
        m_cameraAwaitingPortal = false;
        if (!m_cameraPortal.isNull())
            m_cameraPortal->cancel();
        unpublishTrack(m_cameraCid);
        if (m_cameraPublisherForTest)
            m_cameraCid.clear();
#ifdef HAVE_LIGHTNING_WEBRTC
        clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
#endif
    }
    applyVideoState();
    Q_EMIT mediaStateChanged();
}

void SfuCallController::toggleCamera() { setCameraOn(!m_cameraOn); }

bool SfuCallController::startScreenShare(int pipewireNodeId, int pipewireFd,
                                         quint64 windowHandle,
                                         const QRect &captureRect)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || m_engine.isNull() || !m_client)
        return false;
    if (!sfuSessionLive())
        return false;
    if (pipewireNodeId < 0 && windowHandle == 0 && !captureRect.isValid())
        return false;
    if (m_screenSharing)
        stopScreenShare();
#ifdef Q_OS_LINUX
    m_shareSourceReusable = pipewireFd < 0
        && (windowHandle != 0 || captureRect.isValid());
#else
    m_shareSourceReusable = pipewireFd < 0;
#endif
    m_shareNodeId = pipewireNodeId;
    m_shareWindowHandle = windowHandle;
    m_shareCaptureRect = captureRect;
    const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qCInfo(lcSfuCall) << "screen share publishing node=" << pipewireNodeId
                      << "window=" << (windowHandle != 0)
                      << "x11rect=" << captureRect.isValid()
                      << "encrypted=" << m_roomEncrypted;
    m_engine->setShareQuality(m_settings ? m_settings->effectiveShareMaxHeight()
                                         : SfuMediaEngine::kScreenHeight,
                              m_settings ? m_settings->effectiveShareFps() : 30,
                              m_settings ? m_settings->shareBitrateCapKbps()
                                         : 0);
    const int shareH = m_engine->shareMaxHeight();
    const int shareW = (shareH * 16) / 9;
    m_screenCid = cid;
    m_publishedTrackIds.append(cid);
    m_screenSharing = true;
    const auto declaration = connect(m_engine, &SfuMediaEngine::shareTrackReady,
        this, [this, cid, shareW, shareH](const QString &readyCid,
                                       const QString &backupCid) {
            if (readyCid != cid)
                return;
            if (backupCid.isEmpty())
                m_client->sfuAddTrackFor(m_sfuGeneration, 0, cid,
                    QStringLiteral("screen"), 1, shareW, shareH, true, m_roomEncrypted);
            else
                m_client->sfuAddShareWithBackupFor(m_sfuGeneration, cid,
                    backupCid, shareW, shareH, m_roomEncrypted);
        });
    m_engine->publishVideo(cid,true, pipewireNodeId,
                           pipewireFd, windowHandle, captureRect);
    disconnect(declaration);

    publishShareAudioTrack();
    updateShareAudioAppsPolling();
    ++m_localShareEpoch;
    applyVideoState();
    Q_EMIT mediaStateChanged();
    return true;
#else
    Q_UNUSED(pipewireNodeId); Q_UNUSED(pipewireFd);
    Q_UNUSED(windowHandle); Q_UNUSED(captureRect);
    return false;
#endif
}

void SfuCallController::publishShareAudioTrack()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || !m_shareAudioCid.isEmpty() || m_engine.isNull()
        || !m_client || !sfuSessionLive())
        return;
    if (m_shareAudioMode == 0 || !SfuMediaEngine::shareAudioAvailable())
        return;
    if (m_shareAudioMode == 2 && !shareAudioCanChooseApps())
        return;
    lightning::shareaudio::Selection selection;
    selection.mode = lightning::shareaudio::modeFromInt(m_shareAudioMode);
    selection.keys = m_shareAudioApps;
    if (!selection.capturesAnything()) {
        qCInfo(lcSfuCall) << "screen share audio not published: no "
                             "application chosen";
        return;
    }
    const QString audioCid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qCInfo(lcSfuCall) << "screen share audio publishing encrypted="
                      << m_roomEncrypted << "mode=" << m_shareAudioMode
                      << "chosen=" << m_shareAudioApps.size();
    m_shareAudioCid = audioCid;
    m_publishedTrackIds.append(audioCid);
    m_client->sfuAddTrackFor(m_sfuGeneration, 0, audioCid, QStringLiteral("screenaudio"),
0, 0, 0,
true, m_roomEncrypted);
    resetShareAudioLevel();
    m_engine->setShareAudioMuted(m_shareAudioMuted);
    m_engine->publishShareAudio(audioCid, selection);
    Q_EMIT shareAudioStatusChanged();
#endif
}

void SfuCallController::applyShareAudioToRunningShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || m_engine.isNull() || !m_client)
        return;
    if (!sfuSessionLive())
        return;
    lightning::shareaudio::Selection selection;
    selection.mode = lightning::shareaudio::modeFromInt(m_shareAudioMode);
    selection.keys = m_shareAudioApps;
    if (!selection.capturesAnything()
        || (m_shareAudioMode == 2 && !shareAudioCanChooseApps())
        || !SfuMediaEngine::shareAudioAvailable()) {
        if (!m_shareAudioCid.isEmpty()) {
            qCInfo(lcSfuCall) << "screen share audio withdrawn mid-share";
            unpublishTrack(m_shareAudioCid);
            resetShareAudioLevel();
        }
        return;
    }
    if (m_shareAudioCid.isEmpty()) {
        publishShareAudioTrack();
        return;
    }
    if (m_engine->applyShareAudioSelection(selection))
        return;
    qCInfo(lcSfuCall) << "screen share audio republished for a new choice";
    unpublishTrack(m_shareAudioCid);
    publishShareAudioTrack();
#endif
}

void SfuCallController::resetShareAudioLevel()
{
    const bool changed = m_shareAudioLevelKnown || m_shareAudioHeard
        || !m_shareAudioCarried.isEmpty() || m_shareAudioReportLive;
    m_shareAudioLevelKnown = false;
    m_shareAudioHeard = false;
    m_shareAudioLastHeard.invalidate();
    m_shareAudioCarried.clear();
    m_shareAudioFailed.clear();
    m_shareAudioLimitReached = false;
    m_shareAudioReportLive = false;
    m_shareAudioReportPerApp = false;
    m_shareAudioReportExcludesUs = false;
    if (changed)
        Q_EMIT shareAudioStatusChanged();
}

void SfuCallController::setShareAudioMuted(bool muted)
{
    if (m_shareAudioMuted == muted)
        return;
    m_shareAudioMuted = muted;
    qCInfo(lcSfuCall) << "screen share audio muted=" << muted;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull())
        m_engine->setShareAudioMuted(muted);
#endif
    Q_EMIT mediaStateChanged();
    Q_EMIT shareAudioStatusChanged();
}

void SfuCallController::toggleShareAudioMuted()
{
    setShareAudioMuted(!m_shareAudioMuted);
}

void SfuCallController::shareAudioShareEnded()
{
    resetShareAudioLevel();
    setShareAudioMuted(false);
    m_shareAudioRebuilds = 0;
    if (m_shareAudioRestorePending) {
        m_shareAudioRestorePending = false;
        restoreShareAudioChoice(m_shareAudioRestoreMode,
                                m_shareAudioRestoreApps);
    }
    updateShareAudioAppsPolling();
}

void SfuCallController::restoreShareAudioChoiceAfterShare(
    int mode, const QStringList &keys)
{
    m_shareAudioRestorePending = true;
    m_shareAudioRestoreMode = mode;
    m_shareAudioRestoreApps = keys;
}

void SfuCallController::setShareAudioEnabled(bool on)
{
    setShareAudioMode(on ? (m_shareAudioLastOnMode != 0 ? m_shareAudioLastOnMode
                                                         : 1)
                         : 0);
}

void SfuCallController::setShareAudioMode(int mode)
{
    if (mode != 0 && mode != 1 && mode != 2)
        mode = 0;
    if (mode == 2 && !shareAudioCanChooseApps())
        return;
    if (m_shareAudioMode == mode)
        return;
    m_shareAudioMode = mode;
    if (mode != 0)
        m_shareAudioLastOnMode = mode;
    m_shareAudioRestorePending = false;
    persistShareAudioChoice();
    applyShareAudioToRunningShare();
    updateShareAudioAppsPolling();
    Q_EMIT mediaStateChanged();
    Q_EMIT shareAudioStatusChanged();
    rebuildShareAudioApplications();
}

void SfuCallController::setShareAudioAppChosen(const QString &key,
                                               const QString &label,
                                               bool chosen)
{
    const QString k = key.trimmed().toLower();
    if (k.isEmpty() || !shareAudioCanChooseApps())
        return;
    if (!label.trimmed().isEmpty())
        m_shareAudioAppLabels.insert(k, label.trimmed());
    const bool has = m_shareAudioApps.contains(k);
    if (chosen == has && (!chosen || m_shareAudioMode == 2))
        return;
    if (chosen && !has)
        m_shareAudioApps.append(k);
    else if (!chosen)
        m_shareAudioApps.removeAll(k);
    if (chosen && m_shareAudioMode != 2) {
        m_shareAudioMode = 2;
        m_shareAudioLastOnMode = 2;
    }
    m_shareAudioRestorePending = false;
    persistShareAudioChoice();
    applyShareAudioToRunningShare();
    updateShareAudioAppsPolling();
    Q_EMIT mediaStateChanged();
    Q_EMIT shareAudioStatusChanged();
    rebuildShareAudioApplications();
}

void SfuCallController::chooseOnlyShareAudioApp(const QString &key,
                                                const QString &label)
{
    const QString k = key.trimmed().toLower();
    if (k.isEmpty() || !shareAudioCanChooseApps())
        return;
    if (!label.trimmed().isEmpty())
        m_shareAudioAppLabels.insert(k, label.trimmed());
    restoreShareAudioChoice(2, QStringList{ k });
}

void SfuCallController::restoreShareAudioChoice(int mode,
                                                const QStringList &keys)
{
    if (mode != 0 && mode != 1 && mode != 2)
        mode = 0;
    QStringList normalised;
    for (const QString &key : keys) {
        const QString k = key.trimmed().toLower();
        if (!k.isEmpty() && !normalised.contains(k))
            normalised.append(k);
    }
    if (m_shareAudioMode == mode && m_shareAudioApps == normalised)
        return;
    m_shareAudioMode = mode;
    if (mode != 0)
        m_shareAudioLastOnMode = mode;
    m_shareAudioApps = normalised;
    applyShareAudioToRunningShare();
    updateShareAudioAppsPolling();
    Q_EMIT mediaStateChanged();
    Q_EMIT shareAudioStatusChanged();
    rebuildShareAudioApplications();
}

bool SfuCallController::shareAudioSupported() const
{
    if (m_shareAudioSupportedForTest >= 0)
        return m_shareAudioSupportedForTest == 1;
#ifdef HAVE_LIGHTNING_WEBRTC
    return SfuMediaEngine::shareAudioAvailable();
#else
    return false;
#endif
}

void SfuCallController::refreshShareAudioCapability()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_shareAudioSupportedForTest < 0) {
        lightning::shareaudio::perApplicationCaptureAvailable(true);
    }
#endif
    const bool supported = shareAudioSupported();
    const bool canChoose = shareAudioCanChooseApps();
    if (supported == m_lastShareAudioSupported
        && canChoose == m_lastShareAudioCanChoose) {
        return;
    }
    m_lastShareAudioSupported = supported;
    m_lastShareAudioCanChoose = canChoose;
    Q_EMIT shareAudioCapabilityChanged();
    Q_EMIT shareAudioStatusChanged();
}

bool SfuCallController::shareAudioExcludesOwnPlayback() const
{
    if (m_shareAudioSupportedForTest == 0)
        return false;
#ifdef HAVE_LIGHTNING_WEBRTC
    return SfuMediaEngine::shareAudioSystemExcludesUs();
#else
    return false;
#endif
}

bool SfuCallController::shareAudioCanChooseApps() const
{
    if (m_shareAudioCanChooseForTest >= 0)
        return m_shareAudioCanChooseForTest == 1;
    if (m_shareAudioSupportedForTest == 0)
        return false;
#ifdef HAVE_LIGHTNING_WEBRTC
    return lightning::shareaudio::perApplicationCaptureAvailable();
#else
    return false;
#endif
}

void SfuCallController::watchShareAudioApplications(bool on)
{
    m_shareAudioAppsWatchers = qMax(0, m_shareAudioAppsWatchers + (on ? 1 : -1));
    updateShareAudioAppsPolling();
    if (on)
        refreshShareAudioApplications();
}

void SfuCallController::updateShareAudioAppsPolling()
{
    const bool wanted = m_shareAudioAppsWatchers > 0
        || (m_screenSharing && m_shareAudioMode == 2);
    if (wanted && shareAudioCanChooseApps()) {
        if (!m_shareAudioAppsTimer.isActive())
            m_shareAudioAppsTimer.start();
    } else {
        m_shareAudioAppsTimer.stop();
    }
}

void SfuCallController::refreshShareAudioApplications()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!shareAudioCanChooseApps()) {
        rebuildShareAudioApplications();
        return;
    }
    if (m_shareAudioListInFlight) {
        if (!m_shareAudioListHangLogged && m_shareAudioListStarted.isValid()
            && m_shareAudioListStarted.elapsed() > 10000) {
            m_shareAudioListHangLogged = true;
            qCWarning(lcSfuCall)
                << "share audio: listing the applications has not returned for"
                << m_shareAudioListStarted.elapsed() / 1000
                << "s (a stuck audio daemon?)";
        }
        return;
    }
    m_shareAudioListInFlight = true;
    m_shareAudioListStarted.start();
    lightning::shareaudio::enumerateAsync(
        this, QCoreApplication::applicationPid(),
        SfuMediaEngine::ownAudioClientNames(), {},
        [this](const lightning::shareaudio::Enumeration &e) {
            m_shareAudioListInFlight = false;
            m_shareAudioListHangLogged = false;
            QVariantList apps;
            for (const lightning::shareaudio::Application &a :
                 lightning::shareaudio::groupByApplication(e.streams)) {
                if (a.key.isEmpty())
                    continue;
                m_shareAudioAppLabels.insert(a.key, a.label);
                apps.append(QVariantMap{
                    { QStringLiteral("key"), a.key },
                    { QStringLiteral("label"), a.label },
                    { QStringLiteral("iconName"), a.iconName },
                    { QStringLiteral("active"), a.active },
                    { QStringLiteral("containsUs"), a.containsUs },
                });
            }
            m_shareAudioLastApps = apps;
            rebuildShareAudioApplications();
        });
#else
    rebuildShareAudioApplications();
#endif
}

void SfuCallController::rebuildShareAudioApplications()
{
    QVariantList rows;
    QSet<QString> listed;
    for (const QVariant &v : std::as_const(m_shareAudioLastApps)) {
        const QVariantMap a = v.toMap();
        const QString key = a.value(QStringLiteral("key")).toString();
        if (key.isEmpty() || listed.contains(key))
            continue;
        listed.insert(key);
        rows.append(QVariantMap{
            { QStringLiteral("key"), key },
            { QStringLiteral("label"), a.value(QStringLiteral("label")) },
            { QStringLiteral("iconName"), a.value(QStringLiteral("iconName")) },
            { QStringLiteral("playing"), a.value(QStringLiteral("active")) },
            { QStringLiteral("session"), true },
            { QStringLiteral("chosen"), m_shareAudioApps.contains(key) },
            { QStringLiteral("carried"), m_shareAudioCarried.contains(key) },
            { QStringLiteral("failed"), m_shareAudioFailed.contains(key) },
            { QStringLiteral("containsUs"),
              a.value(QStringLiteral("containsUs")) },
        });
    }
    for (const QString &key : std::as_const(m_shareAudioApps)) {
        if (listed.contains(key))
            continue;
        listed.insert(key);
        rows.append(QVariantMap{
            { QStringLiteral("key"), key },
            { QStringLiteral("label"), m_shareAudioAppLabels.value(key, key) },
            { QStringLiteral("iconName"), QString() },
            { QStringLiteral("playing"), false },
            { QStringLiteral("session"), false },
            { QStringLiteral("chosen"), true },
            { QStringLiteral("carried"), false },
            { QStringLiteral("failed"), m_shareAudioFailed.contains(key) },
            { QStringLiteral("containsUs"), false },
        });
    }
    if (rows == m_shareAudioApplications)
        return;
    m_shareAudioApplications = rows;
    Q_EMIT shareAudioApplicationsChanged();
    Q_EMIT shareAudioStatusChanged();
}

QVariantList SfuCallController::shareAudioApplications() const
{
    return m_shareAudioApplications;
}

QString SfuCallController::shareAudioStatus() const
{
    ShareAudioStatusInput in;
    in.supported = shareAudioSupported();
    if (!in.supported) {
        if (m_shareAudioSupportedForTest == 0) {
            in.why = m_shareAudioWhyForTest;
        } else {
#ifdef HAVE_LIGHTNING_WEBRTC
            using lightning::shareaudio::Unavailability;
            switch (lightning::shareaudio::systemUnavailability()) {
            case Unavailability::MissingPlugin:
                in.why = ShareAudioUnavailable::MissingPlugin;
                break;
            case Unavailability::NoPipeWire:
                in.why = ShareAudioUnavailable::NoPipeWire;
                break;
            case Unavailability::OldWindows:
                in.why = ShareAudioUnavailable::OldWindows;
                break;
            default:
                in.why = ShareAudioUnavailable::NoCapture;
                break;
            }
#endif
        }
    }
    in.mode = m_shareAudioMode;
    for (const QString &key : m_shareAudioApps)
        in.chosenLabels.append(m_shareAudioAppLabels.value(key, key));
    in.trackLive = m_screenSharing && !m_shareAudioCid.isEmpty()
        && m_shareAudioReportLive;
    in.perApplication = in.trackLive && m_shareAudioReportPerApp;
    in.carriedAny = !m_shareAudioCarried.isEmpty();
    in.levelKnown = m_shareAudioLevelKnown;
    in.heard = m_shareAudioHeard;
    for (const QString &key : m_shareAudioFailed)
        in.failedLabels.append(m_shareAudioAppLabels.value(key, key));
    in.limitReached = m_shareAudioLimitReached;
    in.muted = m_shareAudioMuted;
    return shareAudioStatusText(in);
}

QString SfuCallController::shareAudioStatusText(const ShareAudioStatusInput &in)
{
    if (!in.supported) {
        switch (in.why) {
        case ShareAudioUnavailable::MissingPlugin:
            return tr("Screen-share sound is off: this package lacks the audio "
                      "capture plugins it needs.");
        case ShareAudioUnavailable::NoPipeWire:
            return tr("Screen-share sound needs PipeWire; this system has only "
                      "PulseAudio, or PipeWire isn't running.");
        case ShareAudioUnavailable::OldWindows:
            return tr("Screen-share sound needs Windows build 20348 or newer.");
        case ShareAudioUnavailable::NoCapture:
            break;
        }
        return tr("Screen-share sound isn't available on this system.");
    }
    QString what;
    switch (in.mode) {
    case 1:
        what = tr("Sound: everything this computer plays, except Lightning.");
        break;
    case 2:
        if (in.chosenLabels.isEmpty())
            return tr("No apps chosen, so the share has no sound.");
        what = tr("Sound: only %1.").arg(in.chosenLabels.join(
            QStringLiteral(", ")));
        break;
    default:
        return tr("The share has no sound.");
    }
    if (!in.trackLive)
        return what;
    QStringList notes;
    if (in.muted)
        notes << tr("Muted: nobody else hears it.");
    if (!in.failedLabels.isEmpty()) {
        notes << tr("%1 couldn't be captured.")
                     .arg(in.failedLabels.join(QStringLiteral(", ")));
    }
    if (in.limitReached)
        notes << tr("Too many apps to capture at once; some are left out.");
    if (in.perApplication && !in.carriedAny) {
        notes << (in.mode == 2 ? tr("None of them is playing right now.")
                               : tr("No app is playing right now."));
    } else if (in.levelKnown && !in.heard) {
        notes << tr("Nothing is being heard right now.");
    }
    if (notes.isEmpty())
        return what;
    return what + QLatin1Char(' ') + notes.join(QLatin1Char(' '));
}

void SfuCallController::stopScreenShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || m_engine.isNull())
        return;
    unpublishTrack(m_screenCid);
    if (!m_shareAudioCid.isEmpty()) {
        unpublishTrack(m_shareAudioCid);
        m_shareAudioCid.clear();
    }
    m_screenSharing = false;
    shareAudioShareEnded();
    if (m_portal)
        m_portal->cancel();
    clearLocalVideoSurface(SfuMediaEngine::localScreenStreamId());
    applyVideoState();
    Q_EMIT mediaStateChanged();
#endif
}

void SfuCallController::clearLocalVideoSurface(const QString &streamId)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter || streamId.isEmpty())
        return;
    m_videoRouter->deliverFrame(streamId, QVideoFrame());
#else
    Q_UNUSED(streamId);
#endif
}

QVariantMap SfuCallController::ownParticipantRow() const
{
    for (const QVariant &row : m_participants) {
        const QVariantMap participant = row.toMap();
        if (participant.value(QStringLiteral("identity")).toString()
            == m_ownIdentity) {
            return participant;
        }
    }
    return {};
}

void SfuCallController::syncMicMuteToSfu()
{
    if (!m_client || !active())
        return;
    const QVariantMap own = ownParticipantRow();
    if (own.isEmpty())
        return;
    for (const QVariant &t : own.value(QStringLiteral("tracks")).toList()) {
        const QVariantMap track = t.toMap();
        if (track.value(QStringLiteral("source")).toString()
            != QLatin1String("microphone")) {
            continue;
        }
        const QString sid = track.value(QStringLiteral("sid")).toString();
        if (sid.isEmpty())
            return;
        if (track.value(QStringLiteral("muted")).toBool() == m_micMuted)
            return;
        m_client->sfuMuteTrackFor(m_sfuGeneration, 0, sid, m_micMuted);
        return;
    }
}

void SfuCallController::muteOwnTrackIfLive(const QString &source)
{
    if (!m_client || !active() || source.isEmpty())
        return;
    const QVariantMap own = ownParticipantRow();
    if (own.isEmpty())
        return;
    for (const QVariant &t : own.value(QStringLiteral("tracks")).toList()) {
        const QVariantMap track = t.toMap();
        if (track.value(QStringLiteral("source")).toString() != source)
            continue;
        if (track.value(QStringLiteral("muted")).toBool())
            continue;
        const QString sid = track.value(QStringLiteral("sid")).toString();
        if (sid.isEmpty())
            continue;
        m_client->sfuMuteTrackFor(m_sfuGeneration, 0, sid, true);
    }
}

void SfuCallController::applyVideoState()
{
    if (!m_cameraOn)
        muteOwnTrackIfLive(QStringLiteral("camera"));
    if (!m_screenSharing)
        muteOwnTrackIfLive(QStringLiteral("screen_share"));
}

void SfuCallController::unpublishTrack(QString &cid)
{
    if (m_releaseCameraPortalOnUnpublishForTest && !cid.isEmpty()
        && m_engine.isNull()) {
        const QString released = cid;
        onCameraPortalReleased(released);
    }
#ifdef HAVE_LIGHTNING_WEBRTC
    if (cid.isEmpty() || m_engine.isNull())
        return;
    m_engine->unpublish(cid);
    m_publishedTrackIds.removeAll(cid);
    cid.clear();
#else
    Q_UNUSED(cid);
#endif
}

void SfuCallController::setHandRaised(bool raised)
{
    if (m_handRaised == raised)
        return;

    if (m_client && !m_roomId.isEmpty()) {
        QString membership;
        if (m_rtc)
            membership = m_rtc->ownMembershipEventId(m_roomId);
        if (membership.isEmpty())
            membership = m_membershipEventId;
        const quint64 op = m_client->rtcSetHandRaised(
            m_roomId, membership, m_handReactionId, raised);
        if (op != 0)
            m_handOp = op;
    }

    m_handRaised = raised;
    if (m_participantModel && !m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    if (!raised)
        m_handReactionId.clear();
    Q_EMIT mediaStateChanged();
}

void SfuCallController::onHandResult(quint64 opId, bool ok, bool raised,
                                     const QString &category,
                                     const QString &eventId)
{
    if (opId == 0 || opId != m_handOp)
        return;
    m_handOp = 0;
    if (ok) {
        if (raised)
            m_handReactionId = eventId;
        return;
    }
    qCWarning(lcSfuCall) << "raised hand not applied raised=" << raised
                         << "category=" << category;
    m_handRaised = !raised;
    if (m_participantModel && !m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    Q_EMIT mediaStateChanged();
}

void SfuCallController::onHandChanged(const QString &roomId,
                                      const QString &sender,
                                      const QString &membershipEventId,
                                      const QString &reactionEventId,
                                      bool raised)
{
    if (roomId != m_roomId || m_roomId.isEmpty() || !m_participantModel)
        return;

    if (!raised) {
        m_pendingAnnotations.remove(reactionEventId);
        const QString identity = m_handReactions.take(reactionEventId);
        if (identity.isEmpty())
            return;
        m_participantModel->setHandRaised(identity, false);
        if (identity == m_ownIdentity) {
            m_handReactionId.clear();
            if (m_handRaised) {
                m_handRaised = false;
                Q_EMIT mediaStateChanged();
            }
        }
        return;
    }

    if (!m_rtc)
        return;
    const QString identity =
        m_rtc->identityForMembership(roomId, membershipEventId, sender);
    if (identity.isEmpty()) {
        if (!m_rtc->knowsMembership(roomId, membershipEventId)
            && m_pendingAnnotations.size() < kMaxPendingAnnotations) {
            m_pendingAnnotations.insert(
                reactionEventId,
                PendingAnnotation{sender, membershipEventId, QString(),
                                  QDateTime::currentMSecsSinceEpoch()});
        }
        return;
    }
    applyRaisedHand(reactionEventId, identity);
}

void SfuCallController::applyRaisedHand(const QString &reactionEventId,
                                        const QString &identity)
{
    if (!m_participantModel || reactionEventId.isEmpty()
        || identity.isEmpty()) {
        return;
    }
    m_handReactions.insert(reactionEventId, identity);
    m_participantModel->setHandRaised(identity, true);
    if (identity != m_ownIdentity)
        return;
    m_handReactionId = reactionEventId;
    if (!m_handRaised) {
        m_handRaised = true;
        Q_EMIT mediaStateChanged();
    }
}

void SfuCallController::retryPendingAnnotations()
{
    if (m_pendingAnnotations.isEmpty() || !m_rtc || m_roomId.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_pendingAnnotations.begin();
         it != m_pendingAnnotations.end();) {
        if (!it->emoji.isEmpty()
            && now - it->receivedAtMs > m_reactionWindowMs) {
            it = m_pendingAnnotations.erase(it);
            continue;
        }
        const QString identity = m_rtc->identityForMembership(
            m_roomId, it->membershipEventId, it->sender);
        if (identity.isEmpty()) {
            if (m_rtc->knowsMembership(m_roomId, it->membershipEventId))
                it = m_pendingAnnotations.erase(it);
            else
                ++it;
            continue;
        }
        if (it->emoji.isEmpty())
            applyRaisedHand(it.key(), identity);
        else
            applyCallReaction(identity, it->emoji);
        it = m_pendingAnnotations.erase(it);
    }
}

void SfuCallController::onHandsReceived(quint64 opId, const QString &roomId,
                                        const QVariantList &hands)
{
    Q_UNUSED(opId);
    if (roomId != m_roomId || m_roomId.isEmpty() || !m_participantModel)
        return;
    for (const QVariant &value : hands) {
        const QVariantMap hand = value.toMap();
        const QString identity =
            hand.value(QStringLiteral("rtcIdentity")).toString();
        const QString reactionId =
            hand.value(QStringLiteral("reactionEventId")).toString();
        if (identity.isEmpty() || reactionId.isEmpty())
            continue;
        m_pendingAnnotations.remove(reactionId);
        applyRaisedHand(reactionId, identity);
    }
}

void SfuCallController::toggleHandRaised() { setHandRaised(!m_handRaised); }

void SfuCallController::sendCallReaction(const QString &emoji,
                                         const QString &name)
{
    if (emoji.isEmpty() || emoji.toUtf8().size() > 64
        || emoji.contains(QChar(0xFFFD))
        || (emoji.at(0).isLetter() && emoji.at(0).unicode() != 0x2139)
        || emoji.at(0).isSpace()
        || (emoji.at(0).isDigit() && !emoji.endsWith(QChar(0x20E3)))
        || (emoji.at(0).unicode() < 0x80
            && !emoji.endsWith(QChar(0x20E3))))
        return;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, emoji);
    if (finder.toNextBoundary() != emoji.size())
        return;
    if (!active() || m_roomId.isEmpty() || !m_client)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastReactionSentMs != 0
        && now - m_lastReactionSentMs < m_reactionWindowMs) {
        return;
    }

    QString membership;
    if (m_rtc)
        membership = m_rtc->ownMembershipEventId(m_roomId);
    if (membership.isEmpty())
        membership = m_membershipEventId;
    if (membership.isEmpty()) {
        qCWarning(lcSfuCall)
            << "call reaction not sent: no membership event to reference";
        return;
    }

    const quint64 op =
        m_client->rtcSendCallReaction(m_roomId, membership, emoji, name);
    if (op == 0) {
        qCWarning(lcSfuCall) << "call reaction refused before sending";
        return;
    }
    m_reactionOp = op;
    m_lastReactionSentMs = now;
}

void SfuCallController::onRtcSendFinished(quint64 opId, bool ok,
                                          const QString &category,
                                          const QString &eventId)
{
    if (opId != 0 && opId == m_announceOp) {
        m_announceOp = 0;
        const QString pending = m_pendingDeclineTarget;
        m_pendingDeclineTarget.clear();
        if (ok)
            m_announceEventId = eventId;
        if (ok && !pending.isEmpty() && pending == eventId && active())
            endCallDeclinedIfAlone();
        return;
    }
    if (opId == 0 || opId != m_reactionOp)
        return;
    m_reactionOp = 0;
    if (ok)
        return;
    qCWarning(lcSfuCall) << "call reaction not sent category=" << category;
    m_lastReactionSentMs = 0;
}

void SfuCallController::onCallSignal(const CallSignal &signal)
{
    if (signal.kind != CallSignal::Kind::RtcDecline || signal.own
        || !active() || m_roomId.isEmpty() || signal.roomId != m_roomId
        || m_announcePeer.isEmpty() || signal.sender != m_announcePeer
        || signal.targetEventId.isEmpty())
        return;
    if (m_announceEventId.isEmpty()) {
        if (m_announceOp != 0)
            m_pendingDeclineTarget = signal.targetEventId;
        return;
    }
    if (signal.targetEventId != m_announceEventId)
        return;
    endCallDeclinedIfAlone();
}

void SfuCallController::endCallDeclinedIfAlone()
{
    if (m_participantModel) {
        for (int row = 0; row < m_participantModel->rowCount(); ++row) {
            if (!m_participantModel
                     ->data(m_participantModel->index(row),
                            CallParticipantModel::LocalRole)
                     .toBool())
                return;
        }
    }
    qCInfo(lcSfuCall) << "our ring was declined; leaving the call";
    teardown(State::Ended);
    Q_EMIT callFailed(tr("The call was declined."));
}

void SfuCallController::onCallReactionReceived(const QString &roomId,
                                               const QString &sender,
                                               const QString &membershipEventId,
                                               const QString &emoji)
{
    if (roomId != m_roomId || m_roomId.isEmpty() || !m_participantModel)
        return;
    if (emoji.isEmpty() || !m_rtc)
        return;

    const QString identity =
        m_rtc->identityForMembership(roomId, membershipEventId, sender);
    if (identity.isEmpty()) {
        if (!m_rtc->knowsMembership(roomId, membershipEventId)
            && m_pendingAnnotations.size() < kMaxPendingAnnotations) {
            m_pendingAnnotations.insert(
                membershipEventId + QStringLiteral("\x1f") + emoji,
                PendingAnnotation{sender, membershipEventId, emoji,
                                  QDateTime::currentMSecsSinceEpoch()});
        }
        return;
    }
    applyCallReaction(identity, emoji);
}

void SfuCallController::applyCallReaction(const QString &identity,
                                          const QString &emoji)
{
    if (!m_participantModel || identity.isEmpty() || emoji.isEmpty())
        return;
    m_participantModel->setReaction(identity, emoji,
                                    QDateTime::currentMSecsSinceEpoch(),
                                    m_reactionWindowMs);
}

QString SfuCallController::userIdForIdentity(const QString &identity) const
{
    if (identity.isEmpty() || !m_rtc)
        return {};
    return m_rtc->participantForIdentity(m_roomId, identity)
        .value(QStringLiteral("userId"))
        .toString();
}

int SfuCallController::participantVolume(const QString &identity) const
{
    if (!m_settings)
        return 100;
    const QString userId = userIdForIdentity(identity);
    if (userId.isEmpty())
        return 100;
    return m_settings->callParticipantVolume(userId);
}

void SfuCallController::setShareVolume(const QString &shareId, int percent)
{
    const int clamped = qBound(0, percent, 200);
    const QString identity = m_shareModel
        ? m_shareModel->ownerIdentityFor(shareId) : QString();
    if (identity.isEmpty())
        return;
    m_shareVolumes.insert(shareId, clamped);
    const QString ownerUserId = userIdForIdentity(identity);
    if (m_settings && !ownerUserId.isEmpty())
        m_settings->setCallShareVolume(ownerUserId, clamped);
    applyEngineShareVolume(shareId, identity, clamped);
    Q_EMIT shareVolumeChanged(shareId, clamped);
}

bool SfuCallController::shareHasAudio(const QString &shareId) const
{
    const QString identity = m_shareModel
        ? m_shareModel->ownerIdentityFor(shareId) : QString();
    if (identity.isEmpty())
        return false;
    return !trackKeyForSource(identity,
                              QStringLiteral("screen_share_audio")).isEmpty();
}

int SfuCallController::shareVolume(const QString &shareId) const
{
    const auto live = m_shareVolumes.constFind(shareId);
    if (live != m_shareVolumes.constEnd())
        return live.value();
    if (m_settings && m_shareModel) {
        const QString userId =
            userIdForIdentity(m_shareModel->ownerIdentityFor(shareId));
        if (!userId.isEmpty())
            return m_settings->callShareVolume(userId);
    }
    return 100;
}

bool SfuCallController::applyEngineParticipantVolume(const QString &identity,
                                                     int percent)
{
    const QString streamId = streamIdForIdentity(identity);
    if (streamId.isEmpty())
        return false;
    m_engineParticipantVolume.insert(identity, percent);
#ifdef HAVE_LIGHTNING_WEBRTC
    for (auto it = m_subscriberEngines.cbegin();
         it != m_subscriberEngines.cend(); ++it) {
        const QString prefix = QStringLiteral("c%1:").arg(it.key());
        if (streamId.startsWith(prefix) && !it.value().isNull()) {
            it.value()->setParticipantVolume(streamId.mid(prefix.size()), percent);
            return true;
        }
    }
    if (!m_engine.isNull()) {
        m_engine->setParticipantVolume(streamId, percent);
        return true;
    }
#endif
    return false;
}

void SfuCallController::applyEngineShareVolume(const QString &shareId,
                                               const QString &identity,
                                               int percent)
{
    const QString streamId = streamIdForIdentity(identity);
    const QString audioKey =
        trackKeyForSource(identity, QStringLiteral("screen_share_audio"));
    if (streamId.isEmpty() || audioKey.isEmpty())
        return;
    m_engineShareVolume.insert(shareId, percent);
#ifdef HAVE_LIGHTNING_WEBRTC
    for (auto it = m_subscriberEngines.cbegin();
         it != m_subscriberEngines.cend(); ++it) {
        const QString prefix = QStringLiteral("c%1:").arg(it.key());
        if (streamId.startsWith(prefix) && audioKey.startsWith(prefix)
            && !it.value().isNull()) {
            it.value()->setTrackVolume(streamId.mid(prefix.size()),
                                       audioKey.mid(prefix.size()), percent);
            return;
        }
    }
    if (!m_engine.isNull())
        m_engine->setTrackVolume(streamId, audioKey, percent);
#endif
}

void SfuCallController::applyStoredShareVolumes()
{
    if (!m_settings || !m_shareModel)
        return;
    const int count = m_shareModel->rowCount();
    for (int i = 0; i < count; ++i) {
        const QVariantMap share = m_shareModel->get(i);
        const QString shareId = share.value(QStringLiteral("shareId")).toString();
        if (shareId.isEmpty())
            continue;
        const QString identity = m_shareModel->ownerIdentityFor(shareId);
        const QString userId = userIdForIdentity(identity);
        if (userId.isEmpty())
            continue;
        const auto live = m_shareVolumes.constFind(shareId);
        const bool chosenThisCall = live != m_shareVolumes.constEnd();
        if (!chosenThisCall) {
            const int stored = m_settings->callShareVolume(userId);
            if (stored != 100)
                setShareVolume(shareId, stored);
            continue;
        }
        applyEngineShareVolume(shareId, identity, live.value());
    }
}

void SfuCallController::setParticipantVolume(const QString &identity,
                                              int percent)
{
    const int clamped = qBound(0, percent, 200);
    const bool reached = applyEngineParticipantVolume(identity, clamped);
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!reached) {
        qCWarning(lcSfuCall)
            << "participant volume not applied: engine="
            << !m_engine.isNull() << "streamKnown="
            << !streamIdForIdentity(identity).isEmpty()
            << "participants=" << m_participants.size();
    }
#else
    Q_UNUSED(reached);
#endif
    if (m_participantModel)
        m_participantModel->setVolumePercent(identity, clamped);
    const QString userId = userIdForIdentity(identity);
    if (m_settings && !userId.isEmpty())
        m_settings->setCallParticipantVolume(userId, clamped);
}

void SfuCallController::applyStoredVolumes()
{
    if (!m_settings || !m_participantModel)
        return;
    const int count = m_participantModel->rowCount();
    for (int i = 0; i < count; ++i) {
        const QVariantMap person = m_participantModel->get(i);
        if (person.value(QStringLiteral("local")).toBool())
            continue;
        const QString identity =
            person.value(QStringLiteral("identity")).toString();
        const QString userId = userIdForIdentity(identity);
        if (identity.isEmpty() || userId.isEmpty())
            continue;
        const int stored = m_settings->callParticipantVolume(userId);
        if (person.value(QStringLiteral("volumePercent")).toInt() != stored)
            m_participantModel->setVolumePercent(identity, stored);
        applyEngineParticipantVolume(identity, stored);
    }
}

void SfuCallController::rebuildModels()
{
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    if (m_demoCall)
        return;
#endif
    if (!m_participantModel)
        return;
    QVector<CallParticipantRow> rows;
    rows.reserve(m_participants.size() + 1);
    bool sawLocal = false;

    for (const QVariant &value : std::as_const(m_participants)) {
        const QVariantMap entry = value.toMap();
        const QString identity =
            entry.value(QStringLiteral("identity")).toString();
        if (identity.isEmpty())
            continue;
        const QVariantMap person = m_rtc
            ? m_rtc->participantForIdentity(m_roomId, identity)
            : QVariantMap{};
        if (m_rtc && person.isEmpty() && identity != m_ownIdentity
            && entry.value(QStringLiteral("tracks")).toList().isEmpty())
            continue;
        CallParticipantRow row;
        row.identity = identity;
        row.sid = entry.value(QStringLiteral("sid")).toString();
        row.userId = person.value(QStringLiteral("userId")).toString();
        row.deviceId = person.value(QStringLiteral("deviceId")).toString();
        row.displayName =
            person.value(QStringLiteral("displayName")).toString();
        row.avatarMxc = person.value(QStringLiteral("avatarMxc")).toString();
        const bool ownDevice =
            person.value(QStringLiteral("ownDevice")).toBool();
        row.local = ownDevice
            || (!m_ownIdentity.isEmpty() && identity == m_ownIdentity);

        for (const QVariant &t :
             entry.value(QStringLiteral("tracks")).toList()) {
            const QVariantMap track = t.toMap();
            const QString source =
                track.value(QStringLiteral("source")).toString();
            const bool muted = track.value(QStringLiteral("muted")).toBool();
            if (source == QLatin1String("microphone")) {
                row.micKnown = true;
                row.micMuted = muted;
            } else if (source == QLatin1String("camera")) {
                row.cameraKnown = true;
                row.cameraOn = row.cameraOn || !muted;
            } else if (source == QLatin1String("screen_share")) {
                row.screenSharing = row.screenSharing || !muted;
            }
        }
        row.cameraTrackKey =
            trackKeyForSource(identity, QStringLiteral("camera"));
        row.screenTrackKey =
            trackKeyForSource(identity, QStringLiteral("screen_share"));

        if (row.local) {
            sawLocal = true;
            row.micKnown = true;
            row.micMuted = m_micMuted;
            row.cameraKnown = true;
            // Assignment, not `||`: after a stop the SFU may still report the
            // track unmuted, and OR-ing would keep a frozen local share tile.
            row.cameraOn = m_cameraOn;
            row.screenSharing = m_screenSharing;
        }
        rows.append(row);
    }

    if (!sawLocal && !m_ownIdentity.isEmpty()) {
        CallParticipantRow row;
        row.identity = m_ownIdentity;
        row.local = true;
        const QVariantMap person = m_rtc
            ? m_rtc->participantForIdentity(m_roomId, m_ownIdentity)
            : QVariantMap{};
        row.userId = person.value(QStringLiteral("userId")).toString();
        row.deviceId = person.value(QStringLiteral("deviceId")).toString();
        row.displayName =
            person.value(QStringLiteral("displayName")).toString();
        row.avatarMxc = person.value(QStringLiteral("avatarMxc")).toString();
        row.micKnown = true;
        row.micMuted = m_micMuted;
        row.cameraKnown = true;
        row.cameraOn = m_cameraOn;
        row.screenSharing = m_screenSharing;
        rows.append(row);
    }

    m_participantModel->applyParticipants(rows);
    QSet<QString> present;
    for (const CallParticipantRow &row : rows)
        present.insert(row.sid);
    for (auto it = m_speaking.begin(); it != m_speaking.end();) {
        if (present.contains(it.key())) {
            ++it;
        } else {
            m_speakingLevel.remove(it.key());
            it = m_speaking.erase(it);
        }
    }
    m_participantModel->applySpeakers(m_speaking, m_speakingLevel);
    m_participantModel->applyConnectionQuality(m_connectionQuality);
    applyStoredVolumes();
    if (!m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    rebuildShareModel();
    applyStoredShareVolumes();
}

void SfuCallController::rebuildShareModel()
{
    if (!m_shareModel || !m_participantModel)
        return;
    QVector<CallShareRow> shares;
    const int count = m_participantModel->rowCount();
    for (int i = 0; i < count; ++i) {
        const QVariantMap person = m_participantModel->get(i);
        if (!person.value(QStringLiteral("screenSharing")).toBool())
            continue;
        CallShareRow share;
        share.ownerIdentity =
            person.value(QStringLiteral("identity")).toString();
        share.ownerDisplayName =
            person.value(QStringLiteral("displayName")).toString();
        share.trackKey =
            person.value(QStringLiteral("screenTrackKey")).toString();
        share.local = person.value(QStringLiteral("local")).toBool();
        if (share.local) {
            share.shareId = QStringLiteral("local:%1").arg(m_localShareEpoch);
        } else {
            share.shareId = share.trackKey;
        }
        if (share.shareId.isEmpty())
            continue;
        shares.append(share);
    }
    m_shareModel->applyShares(shares);
}

int SfuCallController::participantCount() const
{
    return m_participantModel ? m_participantModel->rowCount() : 0;
}

QVariantList SfuCallController::participants() const
{
    return m_participantModel ? m_participantModel->toVariantList()
                              : QVariantList{};
}

void SfuCallController::ingestParticipantsForTest(const QVariantList &updates)
{
    mergeParticipants(updates);
}

void SfuCallController::setMembershipForTest(const QString &roomId,
                                             const QString &delayId)
{
    m_roomId = roomId;
    m_delayId = delayId;
    m_membershipPublished = true;
    m_lastPublishMs = 0;
}

quint64 SfuCallController::beginMembershipPublishForTest(
    const QString &roomId, const QString &focusUrl, bool announce)
{
    if (!m_client)
        return 0;
    supersedeOwedRetractionFor(roomId);
    m_roomId = roomId;
    m_focusUrl = focusUrl;
    m_announceOnPublish = announce;
    m_announceIntent = QStringLiteral("audio");
    setState(State::Preparing);
    m_publishOp = m_client->rtcPublishMembership(roomId, focusUrl,
        QStringLiteral("audio"),
        m_rtc && !m_rtc->ownFocusUrlFor().isEmpty()
            && RtcController::sameFocusUrl(focusUrl, m_rtc->ownFocusUrlFor())
            && !m_rtc->usesLegacyFocusFor(roomId));
    return m_publishOp;
}

void SfuCallController::setCallStateForTest(State state)
{
    m_state = state;
}

void SfuCallController::setLocalMediaStateForTest(bool cameraOn,
                                                  bool screenSharing)
{
    if (screenSharing && !m_screenSharing)
        ++m_localShareEpoch;
    m_cameraOn = cameraOn;
    m_screenSharing = screenSharing;
    applyVideoState();
    rebuildModels();
    Q_EMIT mediaStateChanged();
}

void SfuCallController::ingestSpeakersForTest(const QVariantList &speakers)
{
    mergeSpeakers(speakers);
}

void SfuCallController::ingestConnectionQualityForTest(
    const QVariantList &updates)
{
    QHash<QString, QString> quality;
    for (const QVariant &value : updates) {
        const QVariantMap entry = value.toMap();
        const QString sid = entry.value(QStringLiteral("sid")).toString();
        const QString level =
            entry.value(QStringLiteral("quality")).toString();
        if (sid.isEmpty() || level.isEmpty()
            || level == QLatin1String("unknown")) {
            continue;
        }
        quality.insert(sid, level);
        m_connectionQuality.insert(sid, level);
    }
    if (m_participantModel)
        m_participantModel->applyConnectionQuality(quality);
}

void SfuCallController::setOwnIdentityForTest(const QString &identity)
{
    m_ownIdentity = identity;
    rebuildModels();
}

void SfuCallController::setStreamInfoVisible(bool visible)
{
    visible = visible && active();
    if (m_streamInfoVisible == visible) return;
    m_streamInfoVisible = visible;
    m_streamInfo.clear();
    Q_EMIT streamInfoChanged();
}

void SfuCallController::resetStreamInfoRates()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine) m_engine->resetStreamInfoBaselines();
    for (auto engine : m_subscriberEngines)
        if (engine) engine->resetStreamInfoBaselines();
#endif
}

void SfuCallController::refreshStreamInfo()
{
    if (!m_streamInfoVisible) return;
    QVariantList rows;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine) {
        m_engine->requestStreamInfoRtpStats();
        rows = m_engine->streamInfoStats();
    }
    for (auto engine : m_subscriberEngines) {
        if (!engine) continue;
        engine->requestStreamInfoRtpStats();
        rows.append(engine->streamInfoStats());
    }
    for (QVariant &value : rows) {
        QVariantMap row = value.toMap();
        const QString stream = row.take(QStringLiteral("streamId")).toString();
        const QString track = row.take(QStringLiteral("trackKey")).toString();
        QString participant = row.value(QStringLiteral("outgoing")).toBool()
            ? tr("You") : tr("Participant");
        for (const QVariant &entry : std::as_const(m_participants)) {
            const QVariantMap peer = entry.toMap();
            if (peer.value(QStringLiteral("sid")).toString() != stream) continue;
            const QString identity = peer.value(QStringLiteral("identity")).toString();
            const int index = m_participantModel->indexOfIdentity(identity);
            if (index >= 0) {
                const auto person = m_participantModel->get(index);
                if (!row.value(QStringLiteral("outgoing")).toBool())
                    row.insert(QStringLiteral("screen"),
                        !track.isEmpty()
                        && person.value(QStringLiteral("screenTrackKey")).toString() == track);
                QString name = person.value(QStringLiteral("displayName")).toString();
                if (name.isEmpty()) name = person.value(QStringLiteral("userId")).toString();
                if (!name.isEmpty()) participant = name;
            }
            break;
        }
        row.insert(QStringLiteral("participant"), participant);
        value = row;
    }
#endif
    m_streamInfo = rows;
    Q_EMIT streamInfoChanged();
}
