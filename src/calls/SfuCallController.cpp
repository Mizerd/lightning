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
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>
#include <QUuid>
#include <QVariantMap>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "calls/CallParticipantModel.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#include "calls/CameraPortal.h"
#include "calls/RtcController.h"
#include "calls/ScreenCastPortal.h"
// Unconditional: the refusal wording names the capture element even in builds
// without a media engine. Only a constexpr accessor is used, so this adds no
// GStreamer or link dependency.
#include "calls/SfuMediaEngine.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"
// Unconditional for one constexpr: the media-key index bound in
// onMediaKeyReceived() runs in every build and must use the cryptor's own
// ring size. Header-only use; no link dependency.
#include "calls/CallFrameCryptor.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include <QVideoFrame>
#include <QVideoSink>

#include "calls/SfuVideoRouter.h"
#endif

Q_LOGGING_CATEGORY(lcSfuCall, "lightning.calls.group")

namespace {
// Element caps a notification's lifetime at 90 s and the Rust side clamps to
// the same value.
constexpr quint64 kAnnounceLifetimeMs = 90000;
} // namespace

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
// Camera-route inputs (and runningSandboxed() for the share refusal wording);
// compiled only with the media engine to avoid unused-function warnings.

/// Whether this process runs in a sandbox that keeps `/dev/video*` out of
/// reach: Flatpak (`$FLATPAK_ID` or `/.flatpak-info`) or Snap (`$SNAP` and
/// `$SNAP_NAME`). Mirrors src/update/InstallType.cpp rather than linking the
/// update lane into this target.
bool runningSandboxed()
{
#ifdef Q_OS_LINUX
    if (!qEnvironmentVariableIsEmpty("FLATPAK_ID"))
        return true;
    if (QFileInfo::exists(QStringLiteral("/.flatpak-info")))
        return true;
    if (!qEnvironmentVariableIsEmpty("SNAP")
        && !qEnvironmentVariableIsEmpty("SNAP_NAME"))
        return true;
#endif
    return false;
}

/// Whether any V4L2 device node is visible. A presence test, not an
/// enumeration: opening devices to decide a route would light the camera.
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

#endif // HAVE_LIGHTNING_WEBRTC

/// The "still here" heartbeat; restarts the MSC4140 delayed retraction (8 s),
/// so it must stay well inside it.
constexpr int kRefreshIntervalMs = 5000;
/// How often the membership state event is re-published when no delayed
/// retraction is armed, so a crashed client does not linger. Must stay well
/// under MEMBERSHIP_EXPIRY_NO_DELAYED_MS in rust/src/rtc.rs (5 min); change
/// the two together.
constexpr qint64 kMembershipRepublishIntervalMs = 60 * 1000;
/// A refused retraction is retried this many times with doubling delay.
/// Bounded so leaving cannot become an endless background sender.
constexpr int kMaxRetractAttempts = 4;
constexpr int kRetractRetryDelayMs = 2000;
/// Presentation bound on the participant list.
constexpr int kMaxParticipants = 64;
/// Annotations (raises, reactions) waiting for the membership they address.
/// One per participant is the real bound.
constexpr int kMaxPendingAnnotations = kMaxParticipants;
} // namespace

SfuCallController::SfuCallController(QObject *parent) : QObject(parent)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    m_videoRouter = new SfuVideoRouter(this);
#endif
    // Created once for the controller's lifetime: views bind to these
    // pointers, so leaving empties them instead of replacing them.
    m_participantModel = new CallParticipantModel(this);
    // participantCount() reads the model, so its NOTIFY is the model's own
    // signal rather than a list of emit sites to keep complete.
    connect(m_participantModel, &CallParticipantModel::countChanged, this,
            &SfuCallController::participantCountChanged);
    m_shareModel = new CallShareModel(this);
    m_stageState = new CallStageState(this);
    m_stageState->setShareModel(m_shareModel);
    // Our own media state feeds the local rows and changes from many places;
    // every mutator already emits mediaStateChanged, so rebuild on that.
    connect(this, &SfuCallController::mediaStateChanged, this,
            [this] { rebuildModels(); });
    m_refreshTimer.setInterval(kRefreshIntervalMs);
    connect(&m_refreshTimer, &QTimer::timeout, this,
            &SfuCallController::refreshMembership);
    // Also reconcile the key lane on each tick; otherwise a failed
    // distribution's retry is only reached by a changed membership read.
    connect(&m_refreshTimer, &QTimer::timeout, this,
            &SfuCallController::reconcileKeyLane);
    m_retractRetryTimer.setSingleShot(true);
    connect(&m_retractRetryTimer, &QTimer::timeout, this,
            &SfuCallController::retryRetraction);
}

SfuCallController::~SfuCallController()
{
    // Never leave a microphone live or a membership behind. A graceful exit
    // dispatches the retraction here (it may complete during the Rust
    // bridge's shutdown, so AppController::prepareForShutdown() should call
    // leave() explicitly). A killed process sends nothing; only the MSC4140
    // delayed retraction survives that, which is why its absence is reported
    // and compensated for.
    teardown(State::Ended);
}

void SfuCallController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    // A client change is an account change: any call belonged to the old one.
    teardown(State::Idle);
    // Drop any outstanding retraction too: op ids come from each client's own
    // counter, so a stale id could match an unrelated op from the new client.
    // Warn only if a retry was already armed (an attempt had failed); the
    // retraction just dispatched will probably land.
    if (m_retractRetryTimer.isActive()) {
        qCWarning(lcSfuCall)
            << "a FAILED call retraction is being abandoned because the "
               "account changed; that membership is now the server's to "
               "expire";
    }
    m_retractRetryTimer.stop();
    m_retractOp = 0;
    m_retractRoomId.clear();
    m_retractDelayId.clear();
    m_retractAttempts = 0;
    // Same for an abandoned publish parked by the teardown above.
    m_abandonedPublishOp = 0;
    m_abandonedPublishRoomId.clear();
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::rtcMembershipPublished, this,
            &SfuCallController::onMembershipPublished);
    // Failed retractions (offline, 5xx, rate limit) are logged and retried;
    // a leftover membership poisons the call for every other client.
    connect(m_client, &MatrixClient::rtcMembershipRetracted, this,
            &SfuCallController::onMembershipRetracted);
    connect(m_client, &MatrixClient::sfuStateChanged, this,
            &SfuCallController::onSfuState);
    connect(m_client, &MatrixClient::sfuJoined, this,
            &SfuCallController::onSfuJoined);
    connect(m_client, &MatrixClient::sfuParticipantsChanged, this,
            &SfuCallController::onSfuParticipants);
    // LiveKit's TrackPublished: the SFU's confirmation that a declared track
    // is real. Logged so a declared-but-never-published track is visible.
    connect(m_client, &MatrixClient::sfuTrackPublished, this,
            [this](const QString &cid, const QString &sid) {
                m_publishedTrackSids.insert(cid, sid);
                qCInfo(lcSfuCall)
                    << "sfu published our track kind="
                    << (cid == m_audioCid ? "microphone" : "other")
                    << "sid=" << sid;
            });
    connect(m_client, &MatrixClient::sfuSpeakersChanged, this,
            &SfuCallController::onSfuSpeakers);
    // Closed enum ("poor"/"good"/"excellent"/"unknown"); feeds the per-tile
    // quality badge.
    connect(m_client, &MatrixClient::sfuConnectionQuality, this,
            &SfuCallController::onSfuConnectionQuality);
    connect(m_client, &MatrixClient::sfuRemoteDescription, this,
            &SfuCallController::onSfuRemoteDescription);
    connect(m_client, &MatrixClient::sfuRemoteCandidate, this,
            &SfuCallController::onSfuRemoteCandidate);
    connect(m_client, &MatrixClient::rtcMediaKeyReceived, this,
            &SfuCallController::onMediaKeyReceived);
    // Raised hands, in element-call's wire format: our send's result, live
    // changes, and the join-time sweep for hands raised before we arrived.
    connect(m_client, &MatrixClient::rtcHandResult, this,
            &SfuCallController::onHandResult);
    connect(m_client, &MatrixClient::rtcHandChanged, this,
            &SfuCallController::onHandChanged);
    connect(m_client, &MatrixClient::rtcHandsReceived, this,
            &SfuCallController::onHandsReceived);
    // Transient reactions (`io.element.call.reaction`): live changes and our
    // send's result. No backlog sweep: a reaction from before we joined is
    // over.
    connect(m_client, &MatrixClient::rtcCallReactionReceived, this,
            &SfuCallController::onCallReactionReceived);
    connect(m_client, &MatrixClient::rtcSendFinished, this,
            &SfuCallController::onRtcSendFinished);
    // Log key send results (counts only, never the key); a distribution that
    // reached nobody otherwise looks like a dead call.
    connect(m_client, &MatrixClient::rtcMediaKeySent, this,
            [this](quint64, bool ok, const QString &category, int delivered,
                   int keyIndex) {
                if (ok) {
                    qCInfo(lcSfuCall) << "media key sent index=" << keyIndex
                                      << "delivered=" << delivered;
                    // Somebody holds this key; see rotateAndDistributeKey().
                    if (delivered > 0)
                        m_deliveredKeyIndex = keyIndex;
                    return;
                }
                qCWarning(lcSfuCall)
                    << "media key NOT sent index=" << keyIndex
                    << "category=" << category << "delivered=" << delivered;
                // The recorded set means "who holds this key"; after a failed
                // send nobody does, so clear it or the retry sees "unchanged".
                m_lastKeyTargets.clear();
            });
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
    // Keys are addressed via MatrixRTC membership, which arrives independently
    // of the SFU participant list. A peer often appears in the SFU list first,
    // so distribution finds no targets; re-run it when the membership lands.
    // distributeKeyIfNeeded() only acts when the addressable set grew.
    connect(m_rtc, &RtcController::sessionChanged, this,
            [this](const QString &roomId) {
                if (!active() || roomId != m_roomId)
                    return;
                // Bind first, then distribute. Frames name the sender by LiveKit
                // sid while keys are stored under the device; the bind needs
                // the membership that just arrived. Binding only from
                // onSfuParticipants() misses peers the SFU announced earlier.
                noteParticipantIdentities();
                distributeKeyIfNeeded();
                // Retry annotations (raised hands) that arrived before the
                // membership they refer to.
                retryPendingAnnotations();
                // Rebuild rows: names and avatars come from the membership,
                // which often arrives after the SFU announced the joiner.
                // rebuildModels() diffs, so an unchanged read emits nothing.
                rebuildModels();
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
    // Received frames need a destination before the first call.
    m_engine->setVideoRouter(m_videoRouter);
    connect(m_engine, &SfuMediaEngine::localDescription, this,
            &SfuCallController::onEngineLocalDescription);
    connect(m_engine, &SfuMediaEngine::localCandidate, this,
            &SfuCallController::onEngineLocalCandidate);
    connect(m_engine, &SfuMediaEngine::failed, this,
            &SfuCallController::onEngineFailed);
    // Not failed(): onEngineFailed ends the call, and one broken capture
    // device must not.
    connect(m_engine, &SfuMediaEngine::publishFailed, this,
            &SfuCallController::onEnginePublishFailed);
    // A remote stream whose frames are being dropped: surface it instead of a
    // green padlock.
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
                // Per-tile marks read mediaBlockedFor(), re-evaluated on this.
                Q_EMIT participantsChanged();
            });
    // The engine reports a capture delivering nothing audible; the UI tells
    // the user, the only one who can fix it.
    connect(m_engine, &SfuMediaEngine::localAudioSilent, this,
            [this](bool silent, double peakDb) {
                Q_UNUSED(peakDb);
                if (m_microphoneSilent == silent)
                    return;
                m_microphoneSilent = silent;
                Q_EMIT microphoneSilentChanged();
            });
#else
    Q_UNUSED(engine);
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
    // Volumes may change from other surfaces; keep the engine in sync.
    connect(m_settings, &SettingsManager::callParticipantVolumeChanged, this,
            [this](const QString &, int) { applyStoredVolumes(); });
    // Share volumes likewise, so a settings change reaches the live call.
    // Must stay a separate connect, not nested in the lambda above.
    connect(m_settings, &SettingsManager::callShareVolumeChanged, this,
            [this](const QString &, int) { applyStoredShareVolumes(); });
    connect(m_settings, &SettingsManager::microphoneGainChanged, this,
            [this] { applyAudioState(); });
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
                // Guard on still being in a call: the picker is modal to the
                // desktop, not to us. The fd is ours, so every path closes it.
                qCInfo(lcSfuCall) << "screen share portal ready node="
                                  << nodeId << "remote_fd="
                                  << (pipewireFd >= 0);
                if (!active()
                    || !startScreenShare(static_cast<int>(nodeId),
                                         pipewireFd)) {
                    qCWarning(lcSfuCall)
                        << "screen share refused after portal grant active="
                        << active();
                    closePortalFd(pipewireFd);
                }
            });
    connect(m_portal, &ScreenCastPortal::cancelled, this, [] {
        // The user declined; no message needed.
    });
    connect(m_portal, &ScreenCastPortal::failed, this,
            [this](const QString &category) {
                qCWarning(lcSfuCall) << "screen share portal failed category="
                                     << category;
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
        // The fd is ours: every exit either hands it to the engine or closes
        // it.
        qCInfo(lcSfuCall) << "camera portal ready remote_fd="
                          << (pipewireFd >= 0);
        if (!m_cameraAwaitingPortal) {
            // The camera was turned off or superseded while the dialog was
            // open.
            qCInfo(lcSfuCall) << "camera portal grant arrived for a camera "
                                 "that is no longer wanted; releasing";
            closePortalFd(pipewireFd);
            return;
        }
        m_cameraAwaitingPortal = false;
        if (!active() || m_engine.isNull() || !m_client) {
            // The call ended while the dialog was open.
            closePortalFd(pipewireFd);
            abandonPendingCamera();
            return;
        }
        publishCameraTrack(pipewireFd);
        applyVideoState();
        Q_EMIT mediaStateChanged();
    });
    connect(m_cameraPortal, &CameraPortal::cancelled, this, [this] {
        // The user declined. No message, but the control must not stay lit.
        qCInfo(lcSfuCall) << "camera portal declined";
        abandonPendingCamera();
    });
    connect(m_cameraPortal, &CameraPortal::failed, this,
            [this](const QString &category) {
                qCWarning(lcSfuCall) << "camera portal failed category="
                                     << category;
                abandonPendingCamera();
                // Same wording as a device that will not open; to the user it
                // is the same fact.
                Q_EMIT callFailed(
                    userFacingError(QStringLiteral("camera_failed")));
            });
}

SfuCallController::LinuxCameraRoute SfuCallController::linuxCameraRoute(
    bool sandboxed, bool portalUsable, bool directDeviceVisible)
{
    // 1. A sandbox has no device node, so the portal is the only possible
    //    camera, even when probes say it is unusable (its refusal is at least
    //    actionable).
    if (sandboxed)
        return LinuxCameraRoute::Portal;
    // 2. A visible device node: the direct path, as on any normal desktop.
    if (directDeviceVisible)
        return LinuxCameraRoute::Direct;
    // 3. Nothing to open directly, but the portal has a camera.
    if (portalUsable)
        return LinuxCameraRoute::Portal;
    // 4. Direct, including its honest failure.
    return LinuxCameraRoute::Direct;
}

SfuCallController::LinuxShareRoute SfuCallController::linuxShareRoute(
    bool portalAvailable, const QString &platformName,
    const QString &sessionType, const QString &waylandDisplay,
    const QString &x11Display, bool captureElementPresent)
{
    // The portal first: it is safe on Wayland, is what KDE and GNOME use, and
    // provides a picker with previews.
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
    // Asked of the running registry: a missing element would only fail at
    // PLAYING, after the user has picked a source.
    if (!captureElementPresent)
        return LinuxShareRoute::RefuseNoCaptureElement;
    return LinuxShareRoute::FallbackDisplays;
}

QString SfuCallController::linuxShareRefusal(LinuxShareRoute route,
                                             bool sandboxed)
{
    switch (route) {
    case LinuxShareRoute::Portal:
    case LinuxShareRoute::FallbackDisplays:
        return {};
    case LinuxShareRoute::RefuseWaylandNeedsPortal:
        // Names the cause and the fix.
        return tr("Screen sharing on Wayland needs xdg-desktop-portal, and "
                  "it isn't responding. Install or start the portal for your "
                  "desktop — for example xdg-desktop-portal-kde or "
                  "xdg-desktop-portal-gnome — then try again.");
    case LinuxShareRoute::RefuseNoCaptureElement:
        // A sandbox cannot use host GStreamer plugins, and on X11 almost no
        // portal backend offers ScreenCast, so lead with the remedies that
        // work (a non-sandboxed build, or a Wayland session) and mention the
        // portal only conditionally.
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
        // Named from its single definition so the refusal, the probe and the
        // pipeline agree.
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
    // The platform's own rectangle; see the header for why QScreen::geometry()
    // and devicePixelRatio() cannot produce it. `handle()` is null while a
    // screen is being torn down (hot-unplug); refuse rather than guess.
    const QPlatformScreen *platform = screen->handle();
    if (!platform)
        return {};
    return validX11CaptureRect(platform->geometry());
#else
    // Windows and macOS capture by display index and never call this. A Linux
    // Qt without private headers has no sound way to get the rectangle, so
    // the fallback lists no display.
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

bool SfuCallController::populateLinuxDisplaySources()
{
    // Displays only, by decision. ximagesrc can take an `xid`, but without a
    // compositor a window's drawable returns whatever is stacked on top of
    // it, which could leak a window the user did not choose.
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
        // The native rectangle, which is what gets captured. A screen whose
        // native rectangle is unavailable is skipped rather than guessed.
        const QRect rect = nativeScreenRect(screen);
        if (!rect.isValid())
            continue;
        // Same row shape as the Windows picker; no `windowHandle` key marks a
        // display in ScreenSharePicker.qml.
        m_screenShareSources.append(QVariantMap{
            { QStringLiteral("index"), i },
            // The output's platform name ("DP-1", "HDMI-A-1").
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
    const bool portalUsable =
        !m_portal.isNull() && ScreenCastPortal::available();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // No portal here: available() means "can name a screen at all".
    if (!portalUsable) {
        qCWarning(lcSfuCall) << "screen share unavailable portal="
                             << !m_portal.isNull();
        Q_EMIT callFailed(
            tr("Screen sharing isn't available on this desktop."));
        return;
    }
#endif
    // Already sharing: stop first rather than open a second portal session,
    // which would orphan one and be refused as `busy` anyway.
    if (m_screenSharing)
        stopScreenShare();
    if (!m_portal.isNull() && m_portal->busy()) {
        qCInfo(lcSfuCall) << "screen share already being chosen; ignoring";
        return;
    }
    qCInfo(lcSfuCall) << "screen share requested";
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // No portal: Lightning owns the picker. The capture elements take a
    // monitor (gdiscreencapsrc) or display (avfvideosrc) index, plus windows
    // via WindowCaptureSrc on Windows.
    m_screenShareSources.clear();
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QWindow *ownWindow = QGuiApplication::focusWindow();
    const QScreen *ownScreen = ownWindow ? ownWindow->screen() : nullptr;
    if (!ownScreen)
        ownScreen = QGuiApplication::primaryScreen();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        // Physical pixels, which is what gets captured; QScreen::geometry()
        // is device-independent.
        QSize g = screen->geometry().size() * screen->devicePixelRatio();
        // Resolve the capture index by device name: Qt's screen order and
        // gdiscreencapsrc's monitor numbering are independent enumerations.
        // This also yields the real framebuffer size.
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
            // The platform's own display name, as the OS settings show it.
            { QStringLiteral("name"), screen->name() },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"),
              QStringLiteral("%1 x %2").arg(g.width()).arg(g.height()) },
            { QStringLiteral("primary"),
              screen == QGuiApplication::primaryScreen() },
            { QStringLiteral("current"), screen == ownScreen },
        });
    }
    // Windows, via Lightning's own capture element (WindowCaptureSrc.h).
    // Empty off Windows.
    for (const lightning::wincap::WindowInfo &window :
         lightning::wincap::enumerateWindows()) {
        m_screenShareSources.append(QVariantMap{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), window.handle },
            { QStringLiteral("name"), window.title },
            // The application, separately from the caption (a browser
            // window's caption is just the tab title).
            { QStringLiteral("application"), window.application },
            { QStringLiteral("geometry"),
              QStringLiteral("%1 x %2").arg(window.width).arg(window.height) },
            { QStringLiteral("primary"), false },
            { QStringLiteral("current"), false },
        });
    }

    if (m_screenShareSources.isEmpty()) {
        Q_EMIT callFailed(tr("No display is available to share."));
        return;
    }
    Q_EMIT screenShareSourcesChanged();
    // A single display source is not a choice; share it without a dialog.
    if (m_screenShareSources.size() == 1) {
        chooseScreenShareSource(0);
        return;
    }
    Q_EMIT screenShareSourcesAvailable();
#else
    // One decision from the pure predicate, so the ordering (portal first,
    // Wayland refused before X11) is testable.
    const LinuxShareRoute route = linuxShareRoute(
        portalUsable, QGuiApplication::platformName(),
        qEnvironmentVariable("XDG_SESSION_TYPE"),
        qEnvironmentVariable("WAYLAND_DISPLAY"),
        qEnvironmentVariable("DISPLAY"),
        SfuMediaEngine::elementAvailable(
            SfuMediaEngine::x11ScreenCaptureElementName()));
    qCInfo(lcSfuCall) << "screen share route=" << static_cast<int>(route);
    switch (route) {
    case LinuxShareRoute::Portal:
        // Monitors and windows; virtual sources are for remote desktop. The
        // portal draws the dialog.
        m_portal->requestShare(ScreenCastPortal::Monitor
                               | ScreenCastPortal::Window);
        return;
    case LinuxShareRoute::FallbackDisplays:
        // No portal on an X11 session: Lightning draws the same picker as on
        // Windows and macOS.
        if (!populateLinuxDisplaySources()) {
            Q_EMIT callFailed(tr("No display is available to share."));
            return;
        }
        Q_EMIT screenShareSourcesChanged();
        // A single display is not a choice.
        if (m_screenShareSources.size() == 1) {
            chooseScreenShareSource(0);
            return;
        }
        Q_EMIT screenShareSourcesAvailable();
        return;
    case LinuxShareRoute::RefuseWaylandNeedsPortal:
    case LinuxShareRoute::RefuseNoCaptureElement:
    case LinuxShareRoute::RefuseNoDisplayServer:
        // Refuse with the reason and no picker.
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
        return;   // Linux: the portal already chose.
    if (index < 0 || index >= m_screenShareSources.size()) {
        qCWarning(lcSfuCall) << "screen share source out of range";
        cancelScreenShareSelection();
        return;
    }
    // Read the row before clearing the list; it says whether it is a window.
    const QVariantMap chosen = m_screenShareSources.at(index).toMap();
    const quint64 windowHandle =
        chosen.value(QStringLiteral("windowHandle")).toULongLong();
    const int displayIndex = chosen.value(QStringLiteral("index")).toInt();
    const bool isWindow = windowHandle != 0;

    // On the Linux fallback the capture is a root-window rectangle, resolved
    // from the screen name now rather than when the list was built, since an
    // unplugged monitor renumbers the rest.
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
    // Exactly one source kind is meaningful: a display index in the node-id
    // slot (Windows/macOS), a window handle, or the Linux fallback rectangle.
    // No portal remote on these paths, hence fd -1.
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

#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
void SfuCallController::startDemoCall(const QString &roomId,
                                      bool withScreenShare)
{
    // Nothing leaves this process: no membership, no SFU, no capture device.
    // Only presentation state and the participant model are written.
    if (roomId.isEmpty())
        return;
    m_roomId = roomId;
    // Before any emit: mediaStateChanged runs rebuildModels() synchronously,
    // which would otherwise remove these rows.
    m_demoCall = true;

    struct DemoPerson {
        const char *id;
        const char *name;
        bool local;
        bool micMuted;
        bool camera;
        bool hand;
    };
    // Fictional names only; a screenshot must never carry a real account.
    static const DemoPerson kPeople[] = {
        { "@alex:lightning.example",  "Alex Rivera",  true,  false, true,  false },
        { "@maya:lightning.example",  "Maya Chen",    false, false, true,  false },
        { "@jordan:lightning.example","Jordan Blake", false, true,  false, true  },
        { "@sam:lightning.example",   "Sam Okonkwo",  false, false, false, false },
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
        row.local = p.local;
        // Known state, so tiles draw it rather than the unknown placeholder.
        row.micKnown = true;
        row.micMuted = p.micMuted;
        row.cameraKnown = true;
        row.cameraOn = p.camera;
        if (p.camera)
            row.cameraTrackKey = QStringLiteral("TR_demo_cam_") + row.sid;
        rows.append(row);
    }
    if (withScreenShare) {
        // The share rides on a real participant, as in a live call.
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
        // One speaker, so the speaking ring is shown.
        QHash<QString, bool> active;
        QHash<QString, qreal> level;
        active.insert(rows[1].sid, true);
        level.insert(rows[1].sid, 0.62);
        m_participantModel->applySpeakers(active, level);
    }

    m_micMuted = false;
    m_cameraOn = true;
    m_screenSharing = withScreenShare;
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
    // A state with a reason was announced to the user: every
    // teardown(State::Failed, ...) is followed by callFailed(m_lastError).
    if (!error.isEmpty())
        m_failureAnnounced = true;
    Q_EMIT stateChanged();

    // Withdraw an announced failure once a later attempt gets past the gate
    // that refused it (Authorizing or later: the membership was accepted).
    // An empty callFailed() is the codebase's idiom for clearing an error.
    // Not on Preparing, where the same gate may refuse again. Track-level
    // publish failures are not covered.
    const bool pastTheJoinGate = state == State::Authorizing
        || state == State::Connecting || state == State::Connected
        || state == State::Reconnecting;
    if (hadOutstandingFailure && error.isEmpty() && pastTheJoinGate) {
        m_failureAnnounced = false;
        qCInfo(lcSfuCall) << "the previous call failure no longer applies; "
                             "withdrawing it";
        Q_EMIT callFailed(QString());
        return;
    }
    // Idle (account/session reset): forget the failure rather than withdraw
    // it, so it cannot clear an unrelated message later.
    if (state == State::Idle)
        m_failureAnnounced = false;
}

QString SfuCallController::userFacingError(const QString &category) const
{
    // A closed set in, plain wording out; raw categories and server strings
    // never reach the user. The final fallback is a last resort: a test pins
    // every Rust category to its own wording.
    //
    // Two gates answer "forbidden" with opposite remedies:
    //   * the homeserver refuses our `m.call.member` state event (a room
    //     power-level issue; GitHub issue #10);
    //   * the call service's JWT authorisation refuses the connection.
    // Lightning's permission editor cannot set the call-member key, so the
    // wording points to raising the user's power level instead.
    if (category == QLatin1String("membership_forbidden"))
        return tr("You don't have permission to join calls in this room. "
                  "A room admin can raise your power level in it; Lightning "
                  "can't change what call membership itself requires.");
    // `sfu_forbidden` is the same refusal at the websocket upgrade; the log
    // keeps the categories apart.
    if (category == QLatin1String("forbidden")
        || category == QLatin1String("sfu_forbidden"))
        return tr("The calling service refused to connect you to this call.");
    // `unsupported` is a 404 from the SFU's JWT service, which the oldest
    // membership chooses, usually on someone else's infrastructure; it is not
    // about our homeserver. `sfu_not_found`: no LiveKit at that path.
    if (category == QLatin1String("unsupported")
        || category == QLatin1String("sfu_not_found"))
        return tr("The calling service this call uses didn't answer. It's "
                  "chosen by whoever started the call, not by your "
                  "homeserver.");
    // The homeserver lacks what a call needs, in practice the OpenID token
    // endpoint (M_UNRECOGNIZED or 404).
    if (category == QLatin1String("unrecognized")
        || category == QLatin1String("not_found"))
        return tr("Your homeserver doesn't support Matrix calls.");
    if (category == QLatin1String("rate_limited"))
        return tr("Too many attempts. Try again in a moment.");
    // Share audio only: the call and the shared picture keep running, so the
    // wording must not say anything ended.
    if (category == QLatin1String("share_audio_failed"))
        return tr("Your screen is being shared without its sound — the "
                  "audio capture couldn't be started.");
    if (category == QLatin1String("share_audio_unavailable"))
        return tr("Your screen is being shared without its sound — this "
                  "system has no way to capture what it is playing.");
    // The focus resolved to a private address, which is refused by policy:
    // the request carries the user's OpenID token and the host is chosen by
    // another participant (docs/matrixrtc.md). Element has no such policy, so
    // the wording must not blame the network.
    if (category == QLatin1String("focus_unroutable"))
        return tr("This call's service is on a private network address, "
                  "which Lightning won't connect to. Whoever set up the call "
                  "needs to give it an address reachable from the internet.");
    // Failures between `authorized` and `signalling`: the Rust side classifies
    // each (sfu.rs, `classify_ws_error`) so each gets an actionable sentence;
    // the log carries the category verbatim.

    // The name did not resolve (distinct from a private address).
    if (category == QLatin1String("focus_unresolved")
        || category == QLatin1String("focus_resolve_timeout"))
        return tr("Lightning couldn't look up this call's service. Its name "
                  "doesn't resolve, or this network's DNS isn't answering.");
    // A name that only means this machine (`localhost`, `.local`,
    // `.internal`), refused before any lookup.
    if (category == QLatin1String("focus_private_name"))
        return tr("This call's service is advertised under a name that only "
                  "means \"this computer\", so Lightning won't connect to "
                  "it. Whoever set up the call needs to give it a real "
                  "address.");
    // No route, e.g. an IPv6 record without working IPv6.
    if (category == QLatin1String("sfu_unreachable"))
        return tr("This network has no route to the call's service. If "
                  "you're on a VPN or a restricted network, that's the "
                  "first thing to check.");
    // Nothing listening on the published port.
    if (category == QLatin1String("sfu_refused_connection"))
        return tr("The call's service refused the connection — nothing is "
                  "listening at the address it published.");
    // Blocked locally: a firewall, or a sandbox without network access.
    if (category == QLatin1String("connect_blocked"))
        return tr("Something on this computer blocked the connection to the "
                  "call's service — a firewall or a security policy.");
    // The connection was opened and nothing came back inside the budget.
    if (category == QLatin1String("connect_timeout"))
        return tr("The call's service didn't answer in time.");
    // TLS roots are compiled in, so this is about the server's certificate or
    // TLS setup, not this machine's.
    if (category == QLatin1String("tls_failed"))
        return tr("Lightning couldn't open a secure connection to the "
                  "call's service. Its certificate or its TLS setup was "
                  "refused.");
    // Answered, but not with a websocket upgrade: usually a proxy or captive
    // portal in between.
    if (category == QLatin1String("ws_rejected")
        || category == QLatin1String("ws_handshake_failed"))
        return tr("The call's service answered, but not as a call service. "
                  "Something between you and it — a proxy or a sign-in "
                  "portal — may be intercepting the connection.");
    // Generic connection failures. `send_failed` is a websocket write that
    // did not go out; `connect_failed` covers websocket errors this build
    // does not classify.
    if (category == QLatin1String("network")
        || category == QLatin1String("connect_failed")
        || category == QLatin1String("connection_lost")
        || category == QLatin1String("transport_failed")
        || category == QLatin1String("send_failed"))
        return tr("Couldn't connect to the call.");
    if (category == QLatin1String("server_error"))
        return tr("The calling service is having trouble.");
    // "The service answered something unusable": the categories differ only
    // in where it went wrong and share one remedy. The log keeps them apart.
    if (category == QLatin1String("invalid")
        || category == QLatin1String("invalid_transport")
        || category == QLatin1String("invalid_request")
        || category == QLatin1String("focus_url_invalid")
        || category == QLatin1String("ws_frame_too_large")
        || category == QLatin1String("unknown"))
        return tr("This call's service isn't set up correctly, so Lightning "
                  "couldn't connect to it.");
    // Before the startsWith below, which would say "couldn't start".
    if (category == QLatin1String("screen_share_source_closed"))
        return tr("The window you were sharing was closed.");
    if (category.startsWith(QLatin1String("screen_share")))
        return tr("Screen sharing couldn't start.");
    if (category == QLatin1String("camera_source_closed"))
        return tr("Your camera stopped.");
    if (category == QLatin1String("camera_failed"))
        return tr("Your camera isn't available.");
    if (category == QLatin1String("audio_source_failed"))
        return tr("Your microphone isn't available.");
    return tr("The call ended unexpectedly.");
}

QString SfuCallController::joinRefusalMessage(const QString &block)
{
    // Maps RtcController::joinBlockReason's closed set. Three QML surfaces map
    // it too (for a disabled button); this one explains an attempted join that
    // was refused. A test requires every token on all four.
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
    if (block == QLatin1String("media_encryption_unavailable"))
        return tr("This room is encrypted, and encrypted calls aren't "
                  "available yet on this build.");
    // Same remedy as userFacingError("membership_forbidden"): Lightning's
    // permissions screen cannot set what call membership requires.
    if (block == QLatin1String("no_permission"))
        return tr("You don't have permission to start or join calls in this "
                  "room. A room admin can raise your power level in it.");
    return tr("This call can't be joined right now.");
}

bool SfuCallController::startsCallForAnnouncement(
    const QVariantList &participants)
{
    // A stale membership of this very device (left by a killed session) does
    // not make a call; our other devices do count.
    for (const QVariant &row : participants) {
        if (!row.toMap().value(QStringLiteral("ownDevice")).toBool())
            return false;
    }
    return true;
}

bool SfuCallController::join(const QString &roomId, bool withVideo)
{
    if (roomId.isEmpty())
        return false;
    if (!m_client || !m_client->supportsSfu()) {
        setState(State::Failed, tr("This build can't join Matrix calls."));
        return false;
    }
#ifndef HAVE_LIGHTNING_WEBRTC
    setState(State::Failed, tr("This build has no calling media support."));
    return false;
#else
    if (m_engine.isNull()) {
        qCWarning(lcSfuCall) << "join refused: no media engine";
        setState(State::Failed, tr("This build has no calling media support."));
        return false;
    }
    if (!m_rtc) {
        qCWarning(lcSfuCall) << "join refused: no rtc controller";
        setState(State::Failed, tr("Calling isn't ready yet."));
        return false;
    }

    // Every join block refuses, and fails closed. An encrypted room whose
    // media cannot be encrypted must never be joined in the clear, and no
    // membership may be published for a session we cannot join.
    const QString block = m_rtc->joinBlockReason(roomId);
    if (!block.isEmpty()) {
        qCWarning(lcSfuCall) << "join refused: block=" << block;
        setState(State::Failed, joinRefusalMessage(block));
        Q_EMIT callFailed(m_lastError);
        return false;
    }

    // One call at a time: tear the previous one down explicitly.
    if (active())
        teardown(State::Ended);

    // Diagnostics are per call; SFU identities repeat across calls.
    m_rtc->forgetUnresolvedIdentityDiagnostics();

    qCInfo(lcSfuCall) << "join begin encrypted="
                      << m_rtc->roomEncrypted(roomId)
                      << "focus=" << (m_rtc->focusUrlFor(roomId).isEmpty()
                                      ? QStringLiteral("<none>")
                                      : QStringLiteral("<set>"));
    ++m_generation;
    m_roomId = roomId;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
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
    // A blocked-media badge must not outlive its call.
    if (!m_blockedStreams.isEmpty()) {
        m_blockedStreams.clear();
        Q_EMIT remoteMediaBlockedChanged();
    }
    // Nor the microphone notice.
    if (m_microphoneSilent) {
        m_microphoneSilent = false;
        Q_EMIT microphoneSilentChanged();
    }
    // Parked keys are not cleared here: a peer sends its key when it sees our
    // membership, which can precede join(). Only aged entries go; teardown()
    // clears the rest.
    expireParkedKeys();
    m_speaking.clear();
    m_speakingLevel.clear();
    m_connectionQuality.clear();
    // Empty the models rather than replace them, so bound views stay bound.
    if (m_participantModel)
        m_participantModel->clear();
    if (m_shareModel)
        m_shareModel->clear();
    if (m_stageState)
        m_stageState->clear();
    m_publishedTrackIds.clear();
    m_publishedTrackSids.clear();
    // Share ids never repeat, so the per-call map is dropped; the user's
    // preference persists per owner in settings (setCallShareVolume).
    m_shareVolumes.clear();
    // Records of what reached the engine are per call.
    m_engineParticipantVolume.clear();
    m_engineShareVolume.clear();
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_membershipEventId.clear();
    m_membershipPublished = false;
    m_delayId.clear();
    // Clear the reason with the id it explains.
    m_delayedCategory.clear();
    m_ownIdentity.clear();
    m_mediaEncrypted = false;
    m_keyIndex = 0;
    m_candidatesSent = 0;
    m_lastKeyTargets.clear();
    m_lastPublishMs = 0;
    m_refreshOp = 0;
    m_delayedRestartOp = 0;
    // A retraction still being retried for this room is moot: we are
    // re-joining, and the retry would remove the membership about to be
    // created. An attempt already in flight cannot be recalled; its answer is
    // ignored, and the refresh cadence covers the race.
    if (m_retractRoomId == roomId) {
        m_retractRetryTimer.stop();
        m_retractRoomId.clear();
        m_retractDelayId.clear();
        m_retractAttempts = 0;
        m_retractOp = 0;
    }

    // Captured once, so a mid-call room change cannot relax the promise.
    // Unknown counts as encrypted.
    m_roomEncrypted = m_rtc->roomEncrypted(roomId);
    // Armed before any media exists, so probes drop frames without a key.
    m_engine->setEncryptionRequired(m_roomEncrypted);
    m_engine->clearKeys();

    // The focus others advertise, or the homeserver's own; empty is legal.
    m_focusUrl = m_rtc->focusUrlFor(roomId);

    setState(State::Preparing);
    Q_EMIT mediaStateChanged();
    Q_EMIT participantsChanged();

    // Sampled before our membership goes out (afterwards the answer is always
    // "no"): only the first arrival announces the call.
    m_announceOnPublish =
        m_rtc && startsCallForAnnouncement(m_rtc->participants(roomId));
    m_announceIntent = withVideo ? QStringLiteral("video")
                                 : QStringLiteral("audio");

    // Membership first, carrying the focus: others pick their SFU from the
    // oldest membership.
    m_publishOp = m_client->rtcPublishMembership(
        roomId, m_focusUrl, withVideo ? QStringLiteral("video")
                                      : QStringLiteral("audio"));
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
/// The category a refused membership publish is reported under. The
/// homeserver refusing our `m.call.member` state event (a room power level)
/// and the SFU refusing the connection both arrive as `forbidden`; renaming
/// the first here lets userFacingError() give the right remedy and the log
/// name the gate. Other categories pass through unchanged.
QString membershipRefusalCategory(const QString &category)
{
    if (category == QLatin1String("forbidden"))
        return QStringLiteral("membership_forbidden");
    return category;
}
} // namespace

void SfuCallController::onMembershipPublished(quint64 opId, bool ok,
                                              const QString &category,
                                              const QString &eventId,
                                              const QString &delayId,
                                              const QString &delayedCategory)
{
    if (opId == 0)
        return;
    // Why no delayed retraction was armed: endpoint absent (permanent) or
    // refused this once (transient). Recorded before any early return so a
    // refresh answer updates it too.
    if (!delayId.isEmpty())
        m_delayedCategory.clear();
    else if (!delayedCategory.isEmpty())
        m_delayedCategory = delayedCategory;
    // The answer to a publish we abandoned by leaving. If it created a live
    // membership after our retraction, it must be retracted, or a ghost
    // participant keeps receiving keys and "waiting for media".
    if (m_abandonedPublishOp != 0 && opId == m_abandonedPublishOp) {
        const QString room = m_abandonedPublishRoomId;
        m_abandonedPublishOp = 0;
        m_abandonedPublishRoomId.clear();
        // The event id, not `ok`, says whether a membership exists: rtc.rs
        // reports ok=false when the long-expiry write landed and the
        // short-expiry replacement did not.
        if (!ok && eventId.isEmpty()) {
            qCInfo(lcSfuCall)
                << "an abandoned membership publish was refused; nothing to "
                   "retract category=" << category;
            return;
        }
        // Back in the same room: the state key is per (user, device), so the
        // current call's publish replaced it; retracting would remove us.
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
        // Use the delay id from this answer: nothing else holds it.
        m_retractRetryTimer.stop();
        m_retractAttempts = 0;
        dispatchRetraction(room, delayId);
        return;
    }
    // A refresh re-publish: do not re-run the join sequence, but adopt the new
    // delay id (each re-publish arms a new delayed retraction).
    if (m_refreshOp != 0 && opId == m_refreshOp) {
        m_refreshOp = 0;
        if (!active())
            return;
        if (!ok) {
            // Not fatal: the next tick retries and `expires` survives several
            // failures. Logged because a membership that stops refreshing
            // silently drops a participant.
            qCWarning(lcSfuCall)
                << "membership refresh FAILED category=" << category;
            return;
        }
        m_membershipPublished = true;
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
    // Recorded before the failure branch, whose teardown needs to know
    // whether there is anything to retract; see the event-id note above.
    if (ok || !eventId.isEmpty())
        m_membershipPublished = true;
    if (m_state != State::Preparing)
        return; // a reply for a call we already left
    if (!ok) {
        // Logged as well as shown, so the two `forbidden` gates can be told
        // apart (issue #10).
        const QString reported = membershipRefusalCategory(category);
        qCWarning(lcSfuCall)
            << "membership REFUSED by the homeserver category=" << category
            << "reportedAs=" << reported;
        teardown(State::Failed, userFacingError(reported));
        Q_EMIT callFailed(m_lastError);
        return;
    }
    m_membershipEventId = eventId;

    // Announce the call to the room. `m.call.member` is a state event and
    // renders nothing in the timeline; the MessageLike announcement is what
    // CallEventDelegate draws. `notification`, not `ring`: ring makes other
    // clients ring audibly. Related to our membership event so receivers can
    // tie it to this session.
    if (m_announceOnPublish && m_client && !m_membershipEventId.isEmpty()) {
        m_announceOnPublish = false;
        const quint64 op = m_client->rtcNotify(
            m_roomId, QStringLiteral("notification"), m_announceIntent,
            kAnnounceLifetimeMs, m_membershipEventId);
        qCInfo(lcSfuCall) << "call announced to the room op=" << op
                          << "intent=" << m_announceIntent;
    }
    // Empty means no MSC4140: cleanup relies on the membership's short
    // `expires` plus the re-publish cadence in refreshMembership(). Both are
    // required.
    m_delayId = delayId;
    if (delayId.isEmpty()) {
        qCWarning(lcSfuCall)
            << "no MSC4140 delayed retraction armed — an unclean exit will "
               "leave this membership until it expires";
    }
    m_lastPublishMs = QDateTime::currentMSecsSinceEpoch();
    m_refreshTimer.start();

    // Read hands raised before we joined, once per join; the sync handler
    // carries every later change.
    if (m_client && !m_roomId.isEmpty())
        m_client->rtcReadRaisedHands(m_roomId);

    if (m_focusUrl.isEmpty()) {
        teardown(State::Failed,
                 tr("Calling isn't available on this homeserver."));
        Q_EMIT callFailed(m_lastError);
        return;
    }
    setState(State::Authorizing);
    const quint64 connectOp = m_client->sfuConnect(m_focusUrl, m_roomId);
    qCInfo(lcSfuCall) << "sfu connect op=" << connectOp;
    if (connectOp == 0) {
        teardown(State::Failed, tr("Couldn't connect to the call."));
        Q_EMIT callFailed(m_lastError);
    }
}

void SfuCallController::onSfuState(const QString &state,
                                    const QString &category)
{
    qCInfo(lcSfuCall) << "sfu state=" << state << "category=" << category
                      << "active=" << active();
    if (!active())
        return;
    if (state == QLatin1String("authorized")) {
        setState(State::Connecting);
        return;
    }
    if (state == QLatin1String("signalling")) {
        setState(State::Connecting);
        return;
    }
    if (state == QLatin1String("failed")) {
        // Only categories that are an actual answer from the service are
        // logged as "refused"; DNS, TLS and routing failures are not.
        const bool serviceAnswered =
            category == QLatin1String("forbidden")
            || category == QLatin1String("sfu_forbidden")
            || category == QLatin1String("unsupported")
            || category == QLatin1String("sfu_not_found")
            || category == QLatin1String("rate_limited")
            || category == QLatin1String("server_error")
            || category == QLatin1String("ws_rejected")
            || category == QLatin1String("membership_forbidden");
        if (serviceAnswered) {
            // The call service's `forbidden`, not the room's; see
            // membershipRefusalCategory().
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
        // The SFU dropped us; report it rather than silently ending.
        if (m_state == State::Connected || m_state == State::Connecting) {
            teardown(State::Failed, tr("The call ended because the "
                                       "connection was lost."));
            Q_EMIT callFailed(m_lastError);
        }
    }
}

void SfuCallController::onSfuJoined(const QString &identity,
                                     const QVariantList &participants,
                                     const QVariantList &iceServers,
                                     const QByteArray &sifTrailer)
{
    // `inCall` includes our own row (the bridge puts it first).
    qCInfo(lcSfuCall) << "sfu joined inCall=" << participants.size()
                      << "iceServers=" << iceServers.size()
                      << "identity=" << (identity.isEmpty()
                                         ? QStringLiteral("<empty>")
                                         : QStringLiteral("<set>"))
                      << "sifTrailerLen=" << sifTrailer.size()
                      << "active=" << active();
    if (!active())
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    m_ownIdentity = identity;
    // Seed the remote mute record from the join's participant list.
    noteRemoteTrackMutes(participants);
    m_participants = participants.mid(0, kMaxParticipants);
    noteParticipantIdentities();
    rebuildModels();
    if (!m_engine.isNull()) {
        m_engine->start();
        // After start(), which clears any previous trailer (and keeps the
        // keys already received for this call). The trailer marks the blank
        // frames the SFU injects into encrypted tracks; see
        // SfuMediaEngine::framesServerInjected().
        m_engine->setServerInjectedTrailer(sifTrailer);
        m_engine->setIceServers(iceServers);
        applyAudioState();
        publishTracks();
    }
    setState(State::Connecting);
    // Apply keys a peer sent before we got here.
    applyParkedKeys();
    Q_EMIT participantsChanged();
    // The key is minted in publishTracks(), before the first frame.
#else
    Q_UNUSED(identity); Q_UNUSED(participants); Q_UNUSED(iceServers);
#endif
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
    // Key before the first frame: in an encrypted room a probe without a key
    // drops our own audio.
    if (m_roomEncrypted)
        rotateAndDistributeKey();
    // The track id is client-chosen and declared before negotiation;
    // declaring and publishing must use the same id.
    const QString audioCid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_client->sfuAddTrack(audioCid, QStringLiteral("microphone"), 0,
                          /*width=*/0, /*height=*/0, false, m_roomEncrypted);
    m_engine->publishAudio(audioCid);
    m_audioCid = audioCid;
    m_publishedTrackIds.append(audioCid);
    // Declared is not published: warn if LiveKit never confirms the track.
    // Local counters look healthy either way.
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
    if (!active())
        return;
    noteRemoteTrackMutes(updates);
    const bool setChanged = mergeParticipants(updates);
    // Drop badges for streams whose participant left; otherwise the header
    // keeps a warning nobody can be attributed to.
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
    // Any change in the set rotates the key, so a leaver cannot decrypt what
    // follows. Rotating on joins too avoids tracking who is new.
    if (setChanged)
        rotateAndDistributeKey();
    // Track sids arrive with this update, so re-apply mute and video state
    // for all three sources: a mute or stop made before the SFU named the
    // track converges here.
    syncMicMuteToSfu();
    applyVideoState();
    // Read the room's membership: a participant announced by the SFU cannot
    // be sent a key until it is read.
    if (m_rtc && !m_roomId.isEmpty())
        m_rtc->refresh(m_roomId);
}

void SfuCallController::noteRemoteTrackMutes(const QVariantList &updates)
{
    // Bounded: far above a real call's track count.
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
                // First sight: only an already-muted track is news.
                it = m_remoteTrackMuted.insert(trackSid,
                                               RemoteTrackMute{muted, 0});
                if (!muted)
                    continue;
            } else if (it->muted == muted) {
                continue;
            }
            it->muted = muted;
            ++it->changes;
            // Rate-limited per track: the first five, then every fiftieth.
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
    // Compare the identity set, not the count: one update can carry a join
    // and a leave, and a leaver must trigger a key rotation.
    QSet<QString> before;
    for (const QVariant &row : std::as_const(m_participants)) {
        before.insert(
            row.toMap().value(QStringLiteral("identity")).toString());
    }
    // A LiveKit ParticipantUpdate is a delta (it includes our own row), so
    // merge by identity and remove on DISCONNECTED, like livekit-client's
    // `Room.handleParticipantUpdates`. Assigning it over the list dropped
    // everyone it did not mention.
    for (const QVariant &value : updates) {
        const QVariantMap row = value.toMap();
        const QString identity =
            row.value(QStringLiteral("identity")).toString();
        if (identity.isEmpty())
            continue;
        int at = -1;
        for (int i = 0; i < m_participants.size(); ++i) {
            if (m_participants.at(i).toMap()
                    .value(QStringLiteral("identity")).toString()
                == identity) {
                at = i;
                break;
            }
        }
        if (row.value(QStringLiteral("state")).toString()
            == QLatin1String("disconnected")) {
            if (at >= 0)
                m_participants.removeAt(at);
            continue;
        }
        if (at >= 0)
            m_participants[at] = row;
        else if (m_participants.size() < kMaxParticipants)
            m_participants.append(row);
    }
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
    if (!active())
        return;
    m_speaking.clear();
    m_speakingLevel.clear();
    for (const QVariant &value : speakers) {
        const QVariantMap entry = value.toMap();
        const QString sid = entry.value(QStringLiteral("sid")).toString();
        if (sid.isEmpty())
            continue;
        m_speaking.insert(sid,
                          entry.value(QStringLiteral("active")).toBool());
        // LiveKit's SpeakerInfo `level` (0..1). Absent stays absent; the model
        // treats it as 0.0 rather than inventing an amplitude.
        if (entry.contains(QStringLiteral("level"))) {
            m_speakingLevel.insert(
                sid, entry.value(QStringLiteral("level")).toDouble());
        }
    }
    // Per-row dataChanged on the speaking roles only; participantsChanged()
    // would rebuild every tile and VideoOutput on every syllable.
    if (m_participantModel)
        m_participantModel->applySpeakers(m_speaking, m_speakingLevel);
}

void SfuCallController::onSfuConnectionQuality(const QVariantList &updates)
{
    if (!active())
        return;
    QHash<QString, QString> quality;
    for (const QVariant &value : updates) {
        const QVariantMap entry = value.toMap();
        const QString sid = entry.value(QStringLiteral("sid")).toString();
        const QString level =
            entry.value(QStringLiteral("quality")).toString();
        if (sid.isEmpty() || level.isEmpty()
            || level == QLatin1String("unknown")) {
            continue; // unknown is the default, not a value to render
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
    if (!active() || m_engine.isNull())
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
    if (!active() || m_engine.isNull())
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
    m_client->sfuLocalDescription(
        kind,
        target == 0 ? QStringLiteral("publisher")
                    : QStringLiteral("subscriber"),
        sdp);
}

void SfuCallController::onEngineLocalCandidate(int target,
                                                const QString &candidateInit)
{
    // Counted, not printed: candidates carry host IPs.
    ++m_candidatesSent;
    if (m_candidatesSent <= 3 || m_candidatesSent % 10 == 0) {
        qCInfo(lcSfuCall) << "local candidate #" << m_candidatesSent
                          << "target=" << target
                          << "active=" << active();
    }
    if (!active() || !m_client)
        return;
    m_client->sfuLocalCandidate(
        target == 0 ? QStringLiteral("publisher")
                    : QStringLiteral("subscriber"),
        candidateInit);
}

// Whether an engine failure concerns only the share's sound. Everything else
// is about the session's own media and ends the call.
bool SfuCallController::categoryIsShareAudioOnly(const QString &category)
{
    return category == QLatin1String("share_audio_failed")
        || category == QLatin1String("share_audio_unavailable");
}

void SfuCallController::onEngineFailed(const QString &category)
{
    qCWarning(lcSfuCall) << "engine failed category=" << category
                         << "active=" << active();
    if (!active())
        return;
    // Share audio is an optional track: turn it off, say so, and keep the call
    // and the picture. Not routed through onEnginePublishFailed, which keys on
    // a cid, because publishShareAudio can fail before a bin exists.
    if (categoryIsShareAudioOnly(category)) {
        if (!m_shareAudioCid.isEmpty()) {
            unpublishTrack(m_shareAudioCid);
            m_shareAudioCid.clear();
        }
        Q_EMIT mediaStateChanged();
        // callFailed reports a failure in plain wording; the call stays
        // active.
        Q_EMIT callFailed(userFacingError(category));
        return;
    }
    // Any other media failure ends the call: the user could neither hear nor
    // be heard.
    teardown(State::Failed, userFacingError(category));
    Q_EMIT callFailed(m_lastError);
}

void SfuCallController::onEnginePublishFailed(const QString &cid,
                                              const QString &category)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    qCWarning(lcSfuCall) << "publish failed category=" << category
                         << "camera=" << (cid == m_cameraCid)
                         << "screen=" << (cid == m_screenCid);
    if (!active() || cid.isEmpty())
        return;
    // Turn the control back off and withdraw the track; the call stays up.
    if (cid == m_cameraCid) {
        m_cameraOn = false;
        unpublishTrack(m_cameraCid);
        clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
    } else if (cid == m_screenCid) {
        m_screenSharing = false;
        unpublishTrack(m_screenCid);
        if (m_portal)
            m_portal->cancel();
        clearLocalVideoSurface(SfuMediaEngine::localScreenStreamId());
    } else {
        // A track we no longer own; nothing to do.
        return;
    }
    applyVideoState();
    Q_EMIT mediaStateChanged();
    // A plain-wording notice; the state is unchanged and the call stays
    // active.
    Q_EMIT callFailed(userFacingError(category));
#else
    Q_UNUSED(cid);
    Q_UNUSED(category);
#endif
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

    // One slot per (sender, device, index): a re-send replaces.
    for (ParkedKey &k : m_parkedKeys) {
        if (k.sender == sender && k.deviceId == deviceId && k.index == index) {
            k.roomId = roomId;
            k.keyBase64 = keyBase64;
            k.arrivedMs = now;
            return;
        }
    }
    // Per-device cap, so one peer rotating keys cannot fill the list.
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
    // Total cap evicts from whoever holds the most, so one member cannot push
    // out another peer's key.
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
    // Clear first: onMediaKeyReceived() may park again while we iterate.
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
    // Logged before every early return, so "never arrived" and "discarded"
    // can be told apart. Index only, never the key or the sender.
    qCInfo(lcSfuCall) << "media key received index=" << keyIndex
                      << "forThisRoom=" << (roomId == m_roomId)
                      << "active=" << active();
    // Validation and parking are outside the WebRTC guard: they need no
    // GStreamer and are covered by call-controller-test. Validate before
    // parking, so malformed keys cannot occupy slots.
    //
    // The index bound is the cryptor's ring size (256): matrix-js-sdk
    // rotates key ids modulo 256.
    if (keyIndex < 0 || keyIndex >= CallFrameCryptor::kKeyRingSize)
        return;
    // Sender-chosen bytes: bounded before decoding.
    if (keyBase64.size() > 256)
        return;
    const QByteArray raw =
        QByteArray::fromBase64(keyBase64.toUtf8(),
                               QByteArray::AbortOnBase64DecodingErrors);
    // 16 or 32 raw bytes: element-call mints 16, livekit-client 32; both
    // derive the same AES-128 key via HKDF. See
    // CallFrameCryptor::isSupportedRawKeyLength().
    if (raw.size() != 16 && raw.size() != 32) {
        qCWarning(lcSfuCall) << "media key refused: unsupported length"
                             << raw.size();
        return;
    }

    if (!active() || roomId != m_roomId) {
        // Park keys that arrive before we are in the call: the peer sends its
        // key as soon as it sees our membership, possibly before the room is
        // even set here, and nothing re-sends it. Same idea as matrix-js-sdk's
        // `keysWithoutMatchingRTCMembership`, bounded by age, one slot per
        // (sender, device, index), and a fair total cap. Kept in memory
        // only, never logged, cleared with the call.
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
    m_engine->setInboundKey(ringName, keyIndex, raw);
    // Also alias the ring to the default SFU identity `{user}:{device}` (what
    // clients assume when a membership omits `membershipID`, and what the JWT
    // service assigns), plus the membership-derived identity when known.
    m_engine->noteParticipantIdentity(
        sender + QLatin1Char(':') + claimedDeviceId, ringName);
    if (m_rtc) {
        const QString identity =
            m_rtc->rtcIdentityFor(m_roomId, sender, claimedDeviceId);
        if (!identity.isEmpty())
            m_engine->noteParticipantIdentity(identity, ringName);
    }
    // If the participant list already names their sid, bind it now.
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
    // QML hands a QObject*; a wrong type attaches nothing.
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    // The camera track key first, so camera and screen share feed different
    // surfaces; the participant sid is the fallback when the SFU gives no mid.
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
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    // No participant-sid fallback: that key also carries the camera, and a
    // face in the screen surface is worse than nothing.
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
    // A wrong type or null releases nothing rather than clearing a route.
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
            // The track sid, which the subscriber SDP's msid carries and the
            // engine routes under. Not the `mid`: TrackInfo's mid belongs to
            // the publisher's connection, not our subscriber transceiver.
            const QString sid = track.value(QStringLiteral("sid")).toString();
            if (sid.isEmpty())
                continue;
            // Prefer a live track: a stop is a mute on this wire (there is no
            // unpublish), so a row can carry a muted old share beside the live
            // one. Fall back to the first match.
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

QString SfuCallController::mediaKeyTargets() const
{
    // Targets are the devices actually in the call: the SFU participant list
    // (presence) intersected with the membership (identity -> Matrix device).
    // Membership alone includes ghosts left by clients that died without
    // retracting, which receive the key while the live peer gets nothing.
    if (!m_rtc || m_roomId.isEmpty())
        return QStringLiteral("[]");
    QJsonArray out;
    QSet<QString> seen;
    for (const QVariant &row : std::as_const(m_participants)) {
        const QVariantMap participant = row.toMap();
        const QString identity =
            participant.value(QStringLiteral("identity")).toString();
        if (identity.isEmpty() || identity == m_ownIdentity)
            continue;
        const QVariantMap person =
            m_rtc->participantForIdentity(m_roomId, identity);
        // Skip our own device and any identity not yet resolved; the next
        // update or membership read retries.
        if (person.value(QStringLiteral("ownDevice")).toBool())
            continue;
        const QString userId =
            person.value(QStringLiteral("userId")).toString();
        const QString deviceId =
            person.value(QStringLiteral("deviceId")).toString();
        if (userId.isEmpty() || deviceId.isEmpty())
            continue;
        const QString key = userId + QChar(0x1f) + deviceId;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        QJsonObject target;
        target.insert(QStringLiteral("user_id"), userId);
        target.insert(QStringLiteral("device_id"), deviceId);
        out.append(target);
    }
    return QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact));
}

QString SfuCallController::mediaKeyRingName(const QString &userId,
                                            const QString &deviceId)
{
    // One ring per device: a user's laptop and phone are independent senders
    // and may both use index 0. The unit separator cannot occur in either
    // half.
    if (userId.isEmpty() || deviceId.isEmpty())
        return {};
    return userId + QChar(0x1f) + deviceId;
}

void SfuCallController::noteParticipantIdentities()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_rtc)
        return;
    // Binds the LiveKit sid (what frames carry) to the sending device (what
    // keys name). Re-run on every participant update and key: neither is
    // known at a fixed point, and re-running is the retry.
    for (const QVariant &row : std::as_const(m_participants)) {
        const QVariantMap participant = row.toMap();
        const QString identity =
            participant.value(QStringLiteral("identity")).toString();
        const QString sid = participant.value(QStringLiteral("sid")).toString();
        if (identity.isEmpty() || sid.isEmpty())
            continue;
        // sid -> identity first, unconditionally: both come from the same SFU
        // row, so it needs nothing else, and frames are dropped until it
        // exists.
        m_engine->noteParticipantIdentity(sid, identity);
        // ...and to the sending device when the membership knows it (covers
        // hashed identities). Never guessed: a wrong device would decrypt with
        // another participant's key.
        const QVariantMap person =
            m_rtc->participantForIdentity(m_roomId, identity);
        const QString name = mediaKeyRingName(
            person.value(QStringLiteral("userId")).toString(),
            person.value(QStringLiteral("deviceId")).toString());
        if (!name.isEmpty()) {
            m_engine->noteParticipantIdentity(sid, name);
            continue;
        }
        // The SFU reports a participant our membership store does not know
        // (the sliding-sync store can be incomplete here), so ask the
        // homeserver. Rate-limited inside RtcController.
        m_rtc->refreshFromServer(m_roomId);
    }
#endif
}

void SfuCallController::reconcileKeyLane()
{
    // Only inside a call.
    if (!active())
        return;
    ++m_keyLaneReconciles;
    // Binding makes keys findable for arriving frames; distribution gets our
    // key to peers. Both, in this order.
    noteParticipantIdentities();
    distributeKeyIfNeeded();
}

void SfuCallController::distributeKeyIfNeeded()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || !m_rtc || !m_roomEncrypted)
        return;
    // Compared with what the last distribution reached, so this is idempotent
    // and safe to call on every sessionChanged.
    const QString targets = mediaKeyTargets();
    // Log which guard returned: "unchanged" and "nobody addressable" are
    // different states.
    if (targets == m_lastKeyTargets) {
        qCInfo(lcSfuCall) << "media key: no redistribution, set unchanged";
        return;
    }
    if (targets == QLatin1String("[]")) {
        const int sfuPeers =
            m_participants.size() > 0 ? m_participants.size() - 1 : 0;
        // A withheld key (current index differs from the newest a peer took)
        // also triggers a server refresh: the SFU peer count can be wrong.
        const bool keyWithheld = m_deliveredKeyIndex >= 0
            && m_keyIndex != m_deliveredKeyIndex;
        qCInfo(lcSfuCall) << "media key: no redistribution, nobody addressable"
                          << "sfuPeers=" << sfuPeers
                          << "keyWithheld=" << keyWithheld;
        // Peers exist but none can be named: refresh from the server.
        if (sfuPeers > 0 || keyWithheld)
            m_rtc->refreshFromServer(m_roomId);
        return;
    }
    qCInfo(lcSfuCall) << "media key: addressable set changed, redistributing";
    rotateAndDistributeKey();
#endif
}

void SfuCallController::rotateAndDistributeKey()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || !m_client || !m_engine)
        return;
    if (!m_roomEncrypted)
        return;

    // 32 bytes from the system CSPRNG; never the generic PRNG for keys.
    QByteArray key(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32 *>(key.data()),
        reinterpret_cast<quint32 *>(key.data() + key.size()));

    const int index = (m_keyIndex + 1) % 16;

    // Distribute first, install second, so our frames are never encrypted
    // under a key nobody has yet. An empty target list is fine: a call we are
    // alone in still encrypts.
    const QString targets = mediaKeyTargets();
    // Remembered so distributeKeyIfNeeded() can spot a newly addressable peer.
    // An empty set is not remembered, or the retry would never fire.
    const int targetCount = targets == QLatin1String("[]")
                                ? 0
                                : static_cast<int>(targets.count(QLatin1Char('{')));
    const int sfuPeers =
        m_participants.size() > 0 ? static_cast<int>(m_participants.size()) - 1 : 0;
    // m_lastKeyTargets means "who holds this key"; clear it on an empty round
    // so it stays retryable.
    if (targets != QLatin1String("[]"))
        m_lastKeyTargets = targets;
    else
        m_lastKeyTargets.clear();
    // Both counts: targets=0 with and without SFU peers are different
    // defects. Counts only.
    qCInfo(lcSfuCall) << "media key distributed index=" << index
                      << "targets=" << targetCount
                      << "sfuPeers=" << sfuPeers
                      << "unresolved=" << (sfuPeers - targetCount);
    // Which devices, as user/device pairs (the same class of identifier as the
    // receive side's `ring=` line), so both ends' logs can be compared.
    {
        const QJsonArray rows =
            QJsonDocument::fromJson(targets.toUtf8()).array();
        QStringList named;
        named.reserve(rows.size());
        for (const QJsonValue &row : rows) {
            const QJsonObject o = row.toObject();
            named << (o.value(QStringLiteral("user_id")).toString()
                      + QLatin1Char('/')
                      + o.value(QStringLiteral("device_id")).toString());
        }
        qCInfo(lcSfuCall) << "media key targeted devices="
                          << (named.isEmpty() ? QStringLiteral("<none>")
                                              : named.join(QLatin1String(", ")));
    }
    const quint64 op =
        m_client->rtcSendMediaKey(m_roomId, QString::fromUtf8(key.toBase64()),
                                  index, targets);
    // op 0: the Rust side refused to dispatch and no result callback will
    // come. Forget the set so the next membership read retries.
    if (op == 0) {
        qCWarning(lcSfuCall) << "media key send was not dispatched index=" << index;
        m_lastKeyTargets.clear();
    }

    // Do not adopt a key that reached nobody while a peer still holds the
    // previous one: every later frame would be unreadable while looking
    // healthy here. The very first key (nobody has taken one yet) is always
    // adopted. The key is still installed in the ring for later adoption.
    const bool reachedNobody = (op == 0);
    const bool someoneHoldsOurKey = (m_deliveredKeyIndex >= 0);
    const bool adopt = !(reachedNobody && someoneHoldsOurKey);
    if (!adopt) {
        qCWarning(lcSfuCall)
            << "media key NOT adopted index=" << index
            << "— it reached nobody; still encrypting under index="
            << m_deliveredKeyIndex
            << "which a peer holds. A retry will rotate when someone is"
            << "addressable again.";
    }
    // m_keyIndex is the allocator cursor: it advances even for a withheld key
    // so an index is never reused with different material.
    m_keyIndex = index;
    m_engine->setOutboundKey(index, key, adopt);
    // Best-effort scrub of our copy; the base64 string passed to the bridge is
    // not zeroed.
    key.fill('\0');

    const bool encrypted = m_engine->encryptionActive();
    if (encrypted != m_mediaEncrypted) {
        m_mediaEncrypted = encrypted;
        Q_EMIT mediaStateChanged();
    }
#endif
}

void SfuCallController::refreshMembership()
{
    if (!active() || !m_client || m_roomId.isEmpty())
        return;
    if (!m_delayId.isEmpty()) {
    // MSC4140 handles cleanup: just keep the delayed retraction from firing.
    // The state event is not re-published on this path. The op id is kept so
    // a failure (e.g. 404: the retraction already fired) can be repaired.
        m_delayedRestartOp = m_client->rtcRestartDelayedLeave(m_delayId);
        return;
    }
    // No server-side cleanup: Rust published a short `expires`, so re-publish
    // on a cadence to keep a live participant from ageing out. This also
    // re-arms a delayed retraction if the server has gained one.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // A backwards clock jump counts as "due now".
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
    // Not guarded on an outstanding m_refreshOp: a lost answer must not stop
    // the heartbeat. Only the newest op is tracked (it carries the live delay
    // id).
    m_refreshOp = m_client->rtcPublishMembership(
        m_roomId, m_focusUrl,
        m_withVideo ? QStringLiteral("video") : QStringLiteral("audio"));
    if (m_refreshOp == 0) {
        qCWarning(lcSfuCall)
            << "membership refresh could not be dispatched — this device may "
               "expire out of the call while still connected";
    }
}

void SfuCallController::onMembershipRetracted(quint64 opId, bool ok,
                                              const QString &category)
{
    if (opId == 0)
        return;
    // The bridge routes both `rtc_membership_retracted` and
    // `rtc_delayed_updated` onto this signal; the op id tells them apart.
    if (m_delayedRestartOp != 0 && opId == m_delayedRestartOp) {
        m_delayedRestartOp = 0;
        if (ok)
            return;
        qCWarning(lcSfuCall)
            << "delayed leave restart FAILED category=" << category;
        // The delay id may have been consumed (the server fired the
        // retraction), so our membership is gone for everyone else and every
        // restart 404s. Re-publish, which recreates the membership and arms a
        // fresh delayed retraction.
        m_delayId.clear();
        republishMembership();
        return;
    }
    if (m_retractOp == 0 || opId != m_retractOp)
        return;
    m_retractOp = 0;
    if (ok) {
        qCInfo(lcSfuCall) << "membership retracted attempts="
                          << m_retractAttempts;
        m_retractRoomId.clear();
        m_retractDelayId.clear();
        m_retractAttempts = 0;
        m_retractRetryTimer.stop();
        return;
    }
    qCWarning(lcSfuCall) << "membership retraction FAILED category="
                         << category << "attempt=" << m_retractAttempts;
    // Retry only transient failures.
    const bool transient = category == QLatin1String("network")
        || category == QLatin1String("rate_limited");
    if (!transient || m_retractAttempts >= kMaxRetractAttempts
        || m_retractRoomId.isEmpty()) {
        qCWarning(lcSfuCall)
            << "giving up on the retraction. This device stays in the room's "
               "call membership until the server's delayed retraction fires, "
               "or until the membership expires. Nothing further is sent.";
        m_retractRoomId.clear();
        m_retractDelayId.clear();
        m_retractAttempts = 0;
        return;
    }
    m_retractRetryTimer.start(kRetractRetryDelayMs
                              * (1 << (m_retractAttempts - 1)));
}

void SfuCallController::retryRetraction()
{
    if (m_retractRoomId.isEmpty())
        return;
    dispatchRetraction(m_retractRoomId, m_retractDelayId);
}

void SfuCallController::dispatchRetraction(const QString &roomId,
                                           const QString &delayId)
{
    if (!m_client || roomId.isEmpty())
        return;
    m_retractRoomId = roomId;
    m_retractDelayId = delayId;
    ++m_retractAttempts;
    m_retractOp = m_client->rtcRetractMembership(roomId, delayId);
    if (m_retractOp != 0)
        return;
    // Not dispatched, so no answer will arrive; do not wait for one.
    qCWarning(lcSfuCall)
        << "retraction could not be dispatched — this device will remain in "
           "the room's call membership until it expires";
    m_retractRoomId.clear();
    m_retractDelayId.clear();
    m_retractAttempts = 0;
}

void SfuCallController::leave()
{
    teardown(State::Ended);
}

void SfuCallController::teardown(State finalState, const QString &error)
{
    // Always logged, so a call's end always has a visible reason.
    qCInfo(lcSfuCall) << "teardown state=" << static_cast<int>(finalState)
                      << "error=" << (error.isEmpty()
                                      ? QStringLiteral("<none>") : error);
    ++m_generation;
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

#ifdef HAVE_LIGHTNING_WEBRTC
    // Media first: release devices before anything that can fail or block.
    if (!m_engine.isNull())
        m_engine->stop();
#endif
    if (m_portal)
        m_portal->cancel();
    // Cancel any camera grant in flight (the dialog is modal to the desktop,
    // not to us); clearing the flag stops a racing grant from publishing
    // into a torn-down engine.
    m_cameraAwaitingPortal = false;
    if (m_cameraPortal)
        m_cameraPortal->cancel();
    if (m_client) {
        m_client->sfuDisconnect();
        if (!m_roomId.isEmpty() && m_membershipPublished) {
            // Only when a membership was actually published: retracting after
            // a refused publish would send a second doomed write and a false
            // "stays in the room" warning. The delay id may be empty (no
            // MSC4140, or leave during Preparing). Tracked, bounded and
            // retried; a retry from a previous leave is superseded.
            m_retractRetryTimer.stop();
            m_retractAttempts = 0;
            dispatchRetraction(m_roomId, m_delayId);
        }
    }

    m_roomId.clear();
    m_focusUrl.clear();
    m_membershipEventId.clear();
    m_membershipPublished = false;
    m_delayId.clear();
    // Clear the reason with the id it explains.
    m_delayedCategory.clear();
    m_ownIdentity.clear();
    m_participants.clear();
    m_remoteTrackMuted.clear();
    // A blocked-media badge must not outlive its call.
    if (!m_blockedStreams.isEmpty()) {
        m_blockedStreams.clear();
        Q_EMIT remoteMediaBlockedChanged();
    }
    // Nor the microphone notice.
    if (m_microphoneSilent) {
        m_microphoneSilent = false;
        Q_EMIT microphoneSilentChanged();
    }
    // Key material never outlives the call.
    m_parkedKeys.clear();
    m_speaking.clear();
    m_speakingLevel.clear();
    m_connectionQuality.clear();
    // Empty the models, never replace them, so bound views see removeRows.
    // Stage state (pins, dismissed shares) belongs to this call.
    if (m_participantModel)
        m_participantModel->clear();
    if (m_shareModel)
        m_shareModel->clear();
    if (m_stageState)
        m_stageState->clear();
    m_publishedTrackIds.clear();
    m_publishedTrackSids.clear();
    // Share ids never repeat, so the per-call map is dropped; the user's
    // preference persists per owner in settings (setCallShareVolume).
    m_shareVolumes.clear();
    // Records of what reached the engine are per call.
    m_engineParticipantVolume.clear();
    m_engineShareVolume.clear();
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_cameraOn = false;
    m_screenSharing = false;
    m_handRaised = false;
    m_handReactionId.clear();
    m_handOp = 0;
    m_handReactions.clear();
    m_pendingAnnotations.clear();
    m_reactionOp = 0;
    m_lastReactionSentMs = 0;
    m_mediaEncrypted = false;
#ifdef HAVE_LIGHTNING_WEBRTC
    // Clear the sink table unconditionally: the engine reads it on a
    // streaming thread, and sids can repeat in the next call.
    if (m_videoRouter)
        m_videoRouter->clear();
#endif
    // Mute/deafen intent survives a call; it is cleared only on sign-out.
    setState(finalState, error);
    Q_EMIT mediaStateChanged();
    Q_EMIT participantsChanged();
}

void SfuCallController::applyAudioState()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull())
        return;
    m_engine->setMicrophoneMuted(m_micMuted);
    m_engine->setOutputMuted(m_deafened);
    // Mic gain rides the same path as mute and deafen, applied at session
    // start rather than on `connected`, which arrives after RTP is already
    // flowing.
    m_engine->setMicrophoneGain(m_settings ? m_settings->microphoneGain()
                                           : 100);
#endif
    // Outside the guards: telling the SFU our mute state is signalling and
    // needs no pipeline (and so is testable in call-controller-test). Other
    // clients read mute from the track, and a mute the SFU inferred from
    // silence must be cleared when we unmute.
    syncMicMuteToSfu();
}

void SfuCallController::setMicrophoneMuted(bool muted)
{
    if (m_micMuted == muted)
        return;
    m_micMuted = muted;
    // Unmuting while deafened lifts the deafen too.
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
    // Remember the prior mute so undeafening restores it.
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
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_client)
        return;

    // Choose the camera route. Linux only in practice: elsewhere CameraPortal
    // is unavailable and this resolves to Direct.
    const bool sandboxed = runningSandboxed();
    const bool deviceNode = v4l2DeviceNodeVisible();
    const bool portalWired = !m_cameraPortal.isNull();
    const bool portalUsable = portalWired && CameraPortal::available()
        && CameraPortal::cameraPresent();
    LinuxCameraRoute route =
        linuxCameraRoute(sandboxed, portalUsable, deviceNode);
    // The portal route needs a wired portal object, or a sandboxed build would
    // wait forever on a signal nothing can emit.
    if (route == LinuxCameraRoute::Portal && !portalWired) {
        qCWarning(lcSfuCall)
            << "camera route=portal but no camera portal is wired; falling "
               "back to the direct device";
        route = LinuxCameraRoute::Direct;
    }
    // Always log the route and its inputs, so a silent camera can be traced
    // to "portal never asked", "portal refused" or "pipeline built nothing".
    qCInfo(lcSfuCall) << "camera route="
                      << (route == LinuxCameraRoute::Portal ? "portal"
                                                            : "direct")
                      << "sandboxed=" << sandboxed
                      << "device_node=" << deviceNode
                      << "portal_wired=" << portalWired
                      << "portal_usable=" << portalUsable;

    if (route == LinuxCameraRoute::Direct) {
        publishCameraTrack(/*pipewireFd=*/-1);
        return;
    }

    // The portal answers asynchronously, possibly after a dialog. Nothing is
    // declared to the SFU until it grants, or other clients wait on a track
    // that never arrives.
    m_cameraAwaitingPortal = true;
    m_cameraPortal->requestAccess();
#endif
}

void SfuCallController::publishCameraTrack(int pipewireFd)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_client) {
        closePortalFd(pipewireFd);
        return;
    }
    const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Declare the camera ceiling (see SfuMediaEngine) so the SFU does not
    // infer simulcast.
    m_client->sfuAddTrack(cid, QStringLiteral("camera"), 1,
                          SfuMediaEngine::kCameraWidth,
                          SfuMediaEngine::kCameraHeight,
                          false, m_roomEncrypted);
    // The engine takes ownership of the fd; see SfuMediaEngine::publishVideo().
    m_engine->publishVideo(cid, /*screenShare=*/false, /*nodeId=*/-1,
                           pipewireFd);
    m_cameraCid = cid;
    m_publishedTrackIds.append(cid);
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
    // Tell the SFU and refresh our own row, as setCameraOn(false) does.
    applyVideoState();
    Q_EMIT mediaStateChanged();
}

void SfuCallController::setCameraOn(bool on)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_cameraOn == on || !active() || m_engine.isNull() || !m_client)
        return;
    m_cameraOn = on;
    if (on) {
        // May publish now (direct) or after a portal dialog; `m_cameraOn` is
        // already true so the control responds immediately.
        startCameraCapture();
    } else {
        // A grant still in flight must stop being wanted, or it would publish
        // a camera the user already switched off.
        m_cameraAwaitingPortal = false;
        if (!m_cameraPortal.isNull())
            m_cameraPortal->cancel();
        // Unpublish the camera track by id; "the last published track" may be
        // the screen share.
        unpublishTrack(m_cameraCid);
        // The self-view sink keeps its last frame; see clearLocalVideoSurface().
        clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
    }
    // Tell the SFU too, or other clients keep seeing the camera.
    applyVideoState();
    Q_EMIT mediaStateChanged();
#else
    Q_UNUSED(on);
#endif
}

void SfuCallController::toggleCamera() { setCameraOn(!m_cameraOn); }

bool SfuCallController::startScreenShare(int pipewireNodeId, int pipewireFd,
                                         quint64 windowHandle,
                                         const QRect &captureRect)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || m_engine.isNull() || !m_client)
        return false;
    // Without a source (node id, window handle or X11 rectangle) refuse rather
    // than guess, or the wrong screen gets published. This guard has a twin in
    // SfuMediaEngine::publishVideo; keep them in sync.
    if (pipewireNodeId < 0 && windowHandle == 0 && !captureRect.isValid())
        return false;
    if (m_screenSharing)
        stopScreenShare();
    const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // `window=` and `x11rect=` are booleans, never the values: an HWND and a
    // root rectangle describe the user's desktop.
    qCInfo(lcSfuCall) << "screen share publishing node=" << pipewireNodeId
                      << "window=" << (windowHandle != 0)
                      << "x11rect=" << captureRect.isValid()
                      << "encrypted=" << m_roomEncrypted;
    // Set the quality before declaring the track: AddTrack carries the real
    // layer dimensions and must match what is published.
    m_engine->setShareQuality(m_settings ? m_settings->shareMaxHeight()
                                         : SfuMediaEngine::kScreenHeight,
                              m_settings ? m_settings->shareFps() : 30);
    const int shareH = m_engine->shareMaxHeight();
    const int shareW = (shareH * 16) / 9;
    m_client->sfuAddTrack(cid, QStringLiteral("screen"), 1,
                          shareW, shareH,
                          true, m_roomEncrypted);
    m_engine->publishVideo(cid, /*screenShare=*/true, pipewireNodeId,
                           pipewireFd, windowHandle, captureRect);
    m_screenCid = cid;
    m_publishedTrackIds.append(cid);
    m_screenSharing = true;

    // Share audio is a separate track (LiveKit SCREEN_SHARE_AUDIO), which is
    // how Element renders it and lets it mute and stop independently. kind=0
    // (audio) with screen_share=true maps to SCREEN_SHARE_AUDIO. Only
    // published when a loopback capture is actually available.
    if (m_shareAudioEnabled && SfuMediaEngine::shareAudioAvailable()) {
        const QString audioCid =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        qCInfo(lcSfuCall) << "screen share audio publishing encrypted="
                          << m_roomEncrypted;
        m_client->sfuAddTrack(audioCid, QStringLiteral("screenaudio"),
                              /*kind=*/0, 0, 0,
                              /*screenShare=*/true, m_roomEncrypted);
        // Record the cid before publishing: publishShareAudio() can emit
        // failed() synchronously, re-entering onEngineFailed, whose cleanup
        // keys on this cid.
        m_shareAudioCid = audioCid;
        m_publishedTrackIds.append(audioCid);
        m_engine->publishShareAudio(audioCid);
    }
    // A new share gets a new stage identity (see m_localShareEpoch), so a
    // viewer who dismissed the previous one is offered this one.
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

void SfuCallController::setShareAudioEnabled(bool on)
{
    if (m_shareAudioEnabled == on)
        return;
    m_shareAudioEnabled = on;
    // Not retroactive: read when a share starts; changing tracks mid-share
    // would need a renegotiation.
    Q_EMIT mediaStateChanged();
}

bool SfuCallController::shareAudioSupported() const
{
#ifdef HAVE_LIGHTNING_WEBRTC
    return SfuMediaEngine::shareAudioAvailable();
#else
    return false;
#endif
}

bool SfuCallController::shareAudioExcludesOwnPlayback() const
{
#ifdef HAVE_LIGHTNING_WEBRTC
    // Guarded: without the media engine ShareAudioSources.cpp is not built.
    return lightning::shareaudio::perApplicationCaptureAvailable();
#else
    return false;
#endif
}

void SfuCallController::stopScreenShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || m_engine.isNull())
        return;
    unpublishTrack(m_screenCid);
    // Always stop both halves: share audio outliving the picture would keep
    // broadcasting the desktop's sound.
    if (!m_shareAudioCid.isEmpty()) {
        unpublishTrack(m_shareAudioCid);
        m_shareAudioCid.clear();
    }
    m_screenSharing = false;
    // Close the portal session so the compositor stops capturing.
    if (m_portal)
        m_portal->cancel();
    // Clear the local share surface; see clearLocalVideoSurface().
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
    // A QVideoSink keeps its last frame, so the self-view would show a frozen
    // picture after the bin is gone. A null QVideoFrame invalidates
    // `videoSize`, so QML shows the placeholder again. Complements the row
    // removal in rebuildModels(); the local stream ids are ours alone.
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
    // The microphone mutes in place in both directions. It is published once
    // per call, so our row's first `microphone` track is it. Video uses
    // muteOwnTrackIfLive() instead.
    if (!m_client || !active())
        return;
    // Use the server's track sid, not our cid: MuteTrackRequest names the
    // track as the SFU knows it.
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
            return; // not named yet; the next ParticipantUpdate re-runs this
        // Reconcile against the state the server reports (including a mute it
        // inferred itself), not what we last sent: this converges and cannot
        // loop.
        if (track.value(QStringLiteral("muted")).toBool() == m_micMuted)
            return;
        m_client->sfuMuteTrack(sid, m_micMuted);
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
    // Mute only. A stop is expressed as a mute, and muting is safe because a
    // live track of a stopped source is self-identifying. Unmuting is not:
    // a stopped track stays listed, and its sid cannot be mapped back to our
    // cid, so unmuting could revive a dead track. Video tracks are always
    // published fresh (and start unmuted), so no unmute is ever needed; the
    // microphone has its own path.
    for (const QVariant &t : own.value(QStringLiteral("tracks")).toList()) {
        const QVariantMap track = t.toMap();
        if (track.value(QStringLiteral("source")).toString() != source)
            continue;
        if (track.value(QStringLiteral("muted")).toBool())
            continue;
        const QString sid = track.value(QStringLiteral("sid")).toString();
        if (sid.isEmpty())
            continue; // not named yet; the next ParticipantUpdate re-runs this
        // No `return`: if two live tracks of this source are listed, both
        // must be silenced.
        m_client->sfuMuteTrack(sid, true);
    }
}

void SfuCallController::applyVideoState()
{
    // Tell the SFU a video track stopped. There is no unpublish verb on this
    // wire (only AddTrack and Mute), so a mute is the removal-shaped signal.
    // The load-bearing mechanism is SfuMediaEngine::unpublish() retiring the
    // transceiver; this mute covers the window until renegotiation lands.
    // Mute only, for sources the user turned off; see muteOwnTrackIfLive().
    if (!m_cameraOn)
        muteOwnTrackIfLive(QStringLiteral("camera"));
    if (!m_screenSharing)
        muteOwnTrackIfLive(QStringLiteral("screen_share"));
}

void SfuCallController::unpublishTrack(QString &cid)
{
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

    // element-call's wire format (`src/reactions/useReactionsSender.tsx`):
    // raise is an `m.reaction` with U+1F590 U+FE0F annotating our own
    // `m.call.member` event; lower is a redaction of it. Annotating the
    // membership scopes the hand to this call.
    if (m_client && !m_roomId.isEmpty()) {
        // Prefer RtcController's observed membership id: a refresh replaces
        // the state event, and annotating a superseded one shows nothing.
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
    // Optimistic on our own row only (the round trip is slow); a refusal
    // reverts it in onHandResult. Remote hands are drawn only from events.
    if (m_participantModel && !m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    // Lowering forgets the reaction id immediately; it is spent either way.
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
        // Keep the id: lowering redacts this exact event.
        if (raised)
            m_handReactionId = eventId;
        return;
    }
    // Revert the control. `category` is a sanitized class, never a body.
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
        // Forget a pending raise, or it would go up once its membership
        // arrives.
        m_pendingAnnotations.remove(reactionEventId);
        // A redaction names only what it removed, so attribute it from the
        // ids we hold; unknown redactions are dropped.
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

    // Attribute a raise through the membership it annotates;
    // identityForMembership refuses a sender who does not own it (anyone can
    // annotate any state event).
    if (!m_rtc)
        return;
    const QString identity =
        m_rtc->identityForMembership(roomId, membershipEventId, sender);
    if (identity.isEmpty()) {
        // Two cases return empty. A membership not read yet is a race (the
        // reaction and the membership arrive independently): park it. A read
        // membership owned by someone else is a forgery: drop it.
        if (!m_rtc->knowsMembership(roomId, membershipEventId)
            && m_pendingAnnotations.size() < kMaxPendingAnnotations) {
            // An empty emoji marks a raise; raises and reactions share one
            // bounded store.
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
    // Our own hand: adopt the reaction id so this device can lower it.
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
        // A parked reaction whose window has passed is dropped rather than
        // drawn late. Raises have no window.
        if (!it->emoji.isEmpty()
            && now - it->receivedAtMs > m_reactionWindowMs) {
            it = m_pendingAnnotations.erase(it);
            continue;
        }
        const QString identity = m_rtc->identityForMembership(
            m_roomId, it->membershipEventId, it->sender);
        if (identity.isEmpty()) {
        // A membership that has arrived and refused this sender will never
        // resolve; drop it.
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
    // Join-time sweep: only adds. A hand this pass could not read is not
    // lowered.
    for (const QVariant &value : hands) {
        const QVariantMap hand = value.toMap();
        const QString identity =
            hand.value(QStringLiteral("rtcIdentity")).toString();
        const QString reactionId =
            hand.value(QStringLiteral("reactionEventId")).toString();
        if (identity.isEmpty() || reactionId.isEmpty())
            continue;
        // Resolved here, so the live handler no longer needs it.
        m_pendingAnnotations.remove(reactionId);
        // Includes our own hand from before this join; adopting its reaction
        // id is what lets us lower it.
        applyRaisedHand(reactionId, identity);
    }
}

void SfuCallController::toggleHandRaised() { setHandRaised(!m_handRaised); }

void SfuCallController::sendCallReaction(const QString &emoji,
                                         const QString &name)
{
    if (!active() || m_roomId.isEmpty() || !m_client || emoji.isEmpty())
        return;
    // Outbound rate limit: the same 3 s window the reaction is shown for.
    // Receivers drop a second reaction inside it anyway, so a held or
    // double-clicked control must not produce a stream of events.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastReactionSentMs != 0
        && now - m_lastReactionSentMs < m_reactionWindowMs) {
        return;
    }

    // Reference our own membership event, preferring RtcController's
    // observation (a re-publish replaces the event; element-call matches
    // `e.eventId === membershipEventId`).
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
        // Refused at the bridge; nothing sent, so allow the next press.
        qCWarning(lcSfuCall) << "call reaction refused before sending";
        return;
    }
    m_reactionOp = op;
    m_lastReactionSentMs = now;
    // Not drawn here: our tile lights when the event comes back through sync,
    // like anybody else's.
}

void SfuCallController::onRtcSendFinished(quint64 opId, bool ok,
                                          const QString &category,
                                          const QString &eventId)
{
    Q_UNUSED(eventId);
    // Shared with every RTC send; only our outstanding reaction op is ours.
    if (opId == 0 || opId != m_reactionOp)
        return;
    m_reactionOp = 0;
    if (ok)
        return;
    // `category` is a sanitized class, never a body or the emoji.
    qCWarning(lcSfuCall) << "call reaction not sent category=" << category;
    // Nothing will come back to draw; allow an immediate retry.
    m_lastReactionSentMs = 0;
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

    // Attribution is the security property, shared with raised hands:
    // identityForMembership refuses a sender who does not own the membership,
    // so nobody can put reactions on other people's tiles.
    const QString identity =
        m_rtc->identityForMembership(roomId, membershipEventId, sender);
    if (identity.isEmpty()) {
        // Not read yet: park it in the same bounded store as raises. Read and
        // owned by someone else: a forgery, dropped.
        if (!m_rtc->knowsMembership(roomId, membershipEventId)
            && m_pendingAnnotations.size() < kMaxPendingAnnotations) {
            m_pendingAnnotations.insert(
                // Keyed by membership plus emoji: reactions carry no event id
                // on this side, and the key cannot collide with a raise's.
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
    // The model owns the display window and the duplicate rule (it ignores a
    // reaction while one is playing, like element-call) and clears the row
    // when the window ends.
    m_participantModel->setReaction(identity, emoji,
                                    QDateTime::currentMSecsSinceEpoch(),
                                    m_reactionWindowMs);
}

QString SfuCallController::userIdForIdentity(const QString &identity) const
{
    if (identity.isEmpty() || !m_rtc)
        return {};
    // Resolved through the membership, never by parsing the identity: sticky
    // format identities are hashes. Empty means unknown.
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
    // Persisted per person, not per share id: share ids change on every
    // restart, so the level must follow the owner.
    const QString ownerUserId = userIdForIdentity(identity);
    if (m_settings && !ownerUserId.isEmpty())
        m_settings->setCallShareVolume(ownerUserId, clamped);
    // Applies to the share's audio track, distinct from the microphone. If it
    // is not listed yet, applyStoredShareVolumes() applies the level later.
    applyEngineShareVolume(shareId, identity, clamped);
    Q_EMIT shareVolumeChanged(shareId, clamped);
}

bool SfuCallController::shareHasAudio(const QString &shareId) const
{
    const QString identity = m_shareModel
        ? m_shareModel->ownerIdentityFor(shareId) : QString();
    if (identity.isEmpty())
        return false;
    // The SFU lists which sources a participant actually publishes.
    return !trackKeyForSource(identity,
                              QStringLiteral("screen_share_audio")).isEmpty();
}

int SfuCallController::shareVolume(const QString &shareId) const
{
    // This call's value first, then the stored per-person level; 100 (not 0,
    // which reads as muted) for an untouched share.
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

/// Hands a participant's level to the media engine and records that it did.
/// Unconditional on the value: guarding on the model's value meant a level
/// set before the stream id was known never reached the engine. Idempotent;
/// a repeated value does not even log.
bool SfuCallController::applyEngineParticipantVolume(const QString &identity,
                                                     int percent)
{
    const QString streamId = streamIdForIdentity(identity);
    if (streamId.isEmpty())
        return false;
    // Recorded before the engine is consulted and outside the media guard, so
    // tests without an engine can see this point was reached. See
    // engineParticipantVolumeForTest().
    m_engineParticipantVolume.insert(identity, percent);
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine.isNull()) {
        m_engine->setParticipantVolume(streamId, percent);
        return true;
    }
#endif
    return false;
}

/// Share counterpart: needs the share audio track key as well as the stream
/// id; either being unknown means the track has not appeared yet.
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
    if (!m_engine.isNull())
        m_engine->setTrackVolume(streamId, audioKey, percent);
#endif
}

/// Carries each sharer's stored level onto shares whose audio has appeared.
/// Must run after the share model is rebuilt: a restarted share has a new id.
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
            // Nothing chosen this call: carry the stored level; leave an
            // untouched share at unity.
            const int stored = m_settings->callShareVolume(userId);
            if (stored != 100)
                setShareVolume(shareId, stored);   // records, persists, applies
            continue;
        }
        // Recorded is not applied: the level is recorded before the audio
        // track key is known, so re-apply every pass. Idempotent, and it does
        // not re-emit shareVolumeChanged.
        applyEngineShareVolume(shareId, identity, live.value());
    }
}

void SfuCallController::setParticipantVolume(const QString &identity,
                                              int percent)
{
    const int clamped = qBound(0, percent, 200);
    // Local only. The engine is addressed by LiveKit stream id, which differs
    // from the SFU identity.
    const bool reached = applyEngineParticipantVolume(identity, clamped);
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!reached) {
        // The value is persisted below regardless, so log a miss to keep it
        // from looking like success.
        qCWarning(lcSfuCall)
            << "participant volume not applied: engine="
            << !m_engine.isNull() << "streamKnown="
            << !streamIdForIdentity(identity).isEmpty()
            << "participants=" << m_participants.size();
    }
#else
    Q_UNUSED(reached);
#endif
    // Recorded in the model so the slider can read it back.
    if (m_participantModel)
        m_participantModel->setVolumePercent(identity, clamped);
    // Persisted per user (from the membership). If unknown yet, it applies to
    // this call only rather than being stored under a key never looked up.
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
        // Never the local row: we do not receive ourselves.
        if (person.value(QStringLiteral("local")).toBool())
            continue;
        const QString identity =
            person.value(QStringLiteral("identity")).toString();
        const QString userId = userIdForIdentity(identity);
        if (identity.isEmpty() || userId.isEmpty())
            continue; // unknown person: unity, nothing invented
        const int stored = m_settings->callParticipantVolume(userId);
        // The model write is guarded; the engine apply is not (see
        // applyEngineParticipantVolume()).
        if (person.value(QStringLiteral("volumePercent")).toInt() != stored)
            m_participantModel->setVolumePercent(identity, stored);
        applyEngineParticipantVolume(identity, stored);
    }
}

// One derivation of the participant rows, diffed into the model, which then
// feeds everything else including participants(). Reassigning a JS array was
// a model reset that destroyed every tile and VideoOutput on each speaker
// update.
void SfuCallController::rebuildModels()
{
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // A staged demo call owns the model; reconciling against the (absent) SFU
    // would clear it. Guarded here because every rebuild path arrives here.
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
        // Resolved through the MatrixRTC membership, never by parsing the
        // identity (sticky format identities are base64 hashes).
        const QVariantMap person = m_rtc
            ? m_rtc->participantForIdentity(m_roomId, identity)
            : QVariantMap{};
        CallParticipantRow row;
        row.identity = identity;
        row.sid = entry.value(QStringLiteral("sid")).toString();
        row.userId = person.value(QStringLiteral("userId")).toString();
        // Room-resolved profile; empty means unknown and the tile falls back
        // to initials.
        row.displayName =
            person.value(QStringLiteral("displayName")).toString();
        row.avatarMxc = person.value(QStringLiteral("avatarMxc")).toString();
        // "local" is this device, per the membership; identity equality is
        // the fallback before the membership lands.
        const bool ownDevice =
            person.value(QStringLiteral("ownDevice")).toBool();
        row.local = ownDevice
            || (!m_ownIdentity.isEmpty() && identity == m_ownIdentity);

        // Track state as the SFU reports it; absent means unknown, and the UI
        // renders nothing rather than "not muted".
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
                // OR, not assignment: a stopped (muted) track stays listed
                // beside a live one; matches trackKeyForSource(), which
                // prefers the live sid.
                row.cameraOn = row.cameraOn || !muted;
            } else if (source == QLatin1String("screen_share")) {
                row.screenSharing = row.screenSharing || !muted;
            }
        }
        // Routing keys, so a tile re-attaches when they change (the SFU may
        // announce a participant before naming their tracks).
        row.cameraTrackKey =
            trackKeyForSource(identity, QStringLiteral("camera"));
        row.screenTrackKey =
            trackKeyForSource(identity, QStringLiteral("screen_share"));

        if (row.local) {
            sawLocal = true;
            // Our own state is authoritative locally; the SFU learns it only
            // after a round trip. applyAudioState()/applyVideoState() converge
            // the server towards it.
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

    // Our own row may arrive with a later update; add a placeholder keyed on
    // the same identity so the real row replaces it.
    if (!sawLocal && !m_ownIdentity.isEmpty()) {
        CallParticipantRow row;
        row.identity = m_ownIdentity;
        row.local = true;
        const QVariantMap person = m_rtc
            ? m_rtc->participantForIdentity(m_roomId, m_ownIdentity)
            : QVariantMap{};
        row.userId = person.value(QStringLiteral("userId")).toString();
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
    // Re-apply the last speaker and quality data so new rows are not born
    // stale.
    m_participantModel->applySpeakers(m_speaking, m_speakingLevel);
    m_participantModel->applyConnectionQuality(m_connectionQuality);
    // New rows start at unity; restore the user's stored levels.
    applyStoredVolumes();
    // Re-assert our own hand on the rebuilt local row.
    if (!m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    rebuildShareModel();
    // After rebuildShareModel(), so newly appeared shares get their stored
    // level applied to the engine, not just shown on the slider.
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
            // Our own share exists before the SFU names a track, so it is
            // keyed by m_localShareEpoch; a new share gets a new id.
            share.shareId = QStringLiteral("local:%1").arg(m_localShareEpoch);
        } else {
            // The track sid: a restarted share is a new track and a new id,
            // so a stale dismissal cannot suppress it.
            share.shareId = share.trackKey;
        }
        if (share.shareId.isEmpty())
            continue; // no track stated yet: nothing could attach to the row
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
    // Also record that a membership exists: teardown() only retracts one
    // that does.
    m_membershipPublished = true;
    m_lastPublishMs = 0;
}

quint64 SfuCallController::beginMembershipPublishForTest(
    const QString &roomId, const QString &focusUrl)
{
    // The tail of join(), in the same order and through the same calls, so
    // the answer reaches the production onMembershipPublished().
    if (!m_client)
        return 0;
    m_roomId = roomId;
    m_focusUrl = focusUrl;
    // Tests never announce the call.
    m_announceOnPublish = false;
    setState(State::Preparing);
    m_publishOp = m_client->rtcPublishMembership(roomId, focusUrl,
                                                 QStringLiteral("audio"));
    return m_publishOp;
}

void SfuCallController::setCallStateForTest(State state)
{
    // Only the state, so `active()` gates behave as in a real call.
    m_state = state;
}

void SfuCallController::setLocalMediaStateForTest(bool cameraOn,
                                                  bool screenSharing)
{
    // The engine-independent half of setCameraOn()/startScreenShare()/
    // stopScreenShare(), going through the same applyVideoState().
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
    m_speaking.clear();
    m_speakingLevel.clear();
    for (const QVariant &value : speakers) {
        const QVariantMap entry = value.toMap();
        const QString sid = entry.value(QStringLiteral("sid")).toString();
        if (sid.isEmpty())
            continue;
        m_speaking.insert(sid,
                          entry.value(QStringLiteral("active")).toBool());
        if (entry.contains(QStringLiteral("level"))) {
            m_speakingLevel.insert(
                sid, entry.value(QStringLiteral("level")).toDouble());
        }
    }
    if (m_participantModel)
        m_participantModel->applySpeakers(m_speaking, m_speakingLevel);
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
