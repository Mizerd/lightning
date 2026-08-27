#include "calls/SfuCallController.h"

#include "calls/WindowCaptureSrc.h"

#include <QLoggingCategory>

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#if defined(Q_OS_LINUX) && defined(LIGHTNING_HAVE_QPA_SCREEN)
// The ONLY way to obtain a screen's native, root-relative rectangle — the
// coordinate space the X11 capture element addresses. Private Qt API, taken
// knowingly: no public accessor exposes it, and the arithmetic alternative is
// provably wrong in two independent ways (nativeScreenRect() in the header).
//
// Compiled on Linux alone, so no Windows or macOS translation unit sees a
// private Qt header; and gated on the CMake probe, so a Qt packaged without
// private headers still builds. Without it there is no sound way to get a
// capture rectangle, and the fallback refuses instead of guessing one.
#include <qpa/qplatformscreen.h>
#endif
#include <QDateTime>
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
#include "calls/RtcController.h"
#include "calls/ScreenCastPortal.h"
// UNCONDITIONAL, unlike the HAVE_LIGHTNING_WEBRTC block below: the refusal
// wording names the capture element, and that sentence exists in builds with
// no media engine at all. The header forward-declares its GStreamer types
// rather than including any, and the only member reached from here is a
// constexpr accessor, so this pulls in no GStreamer and no link dependency.
#include "calls/SfuMediaEngine.h"
#include "app/SettingsManager.h"
#include "matrix/MatrixClient.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include <QVideoFrame>
#include <QVideoSink>

#include "calls/CallFrameCryptor.h"
#include "calls/SfuVideoRouter.h"
#endif

Q_LOGGING_CATEGORY(lcSfuCall, "lightning.calls.group")

namespace {
/// Closes a descriptor the desktop portal handed us, and compiles everywhere.
///
/// The portal DUPLICATES the fd for us, so it is ours and every path out —
/// including every refusal — has to close it, or a declined share leaks one
/// per attempt. But there is no xdg-desktop-portal off Unix: `pipewireFd` is
/// always -1 there and nothing is ever open.
///
/// The CALL SITE still has to compile. `<unistd.h>` was guarded with
/// `#ifdef Q_OS_UNIX` while the bare `::close()` below it was not, so on
/// MinGW the header was skipped and `::close` was undeclared — which is
/// exactly how the Windows package build died (pipeline 112,
/// "'::close' has not been declared; did you mean 'fclose'?"). One helper,
/// guarded once, rather than an `#ifdef` around each use.
void closePortalFd(int fd)
{
#ifdef Q_OS_UNIX
    if (fd >= 0)
        ::close(fd);
#else
    Q_UNUSED(fd);
#endif
}

/// The heartbeat that says "still here". It restarts the MSC4140 delayed
/// retraction (8 s), so it has to be comfortably inside that.
constexpr int kRefreshIntervalMs = 5000;
/// How often the membership STATE EVENT itself is re-published when — and
/// only when — no delayed retraction is armed.
///
/// This exists because nothing re-published it at all. The class comment
/// claimed a refresh "well inside" the four-hour expiry and there was no such
/// code: `refreshMembership()` was guarded on a delay id and its only action
/// was restarting the delayed event, so on a homeserver without MSC4140 the
/// entire heartbeat was a no-op and a dead client sat in the call for FOUR
/// HOURS. That is the maintainer's report.
///
/// 60 s against the 5 min expiry Rust publishes in that case: five
/// consecutive failures are survivable. It must stay well under that expiry —
/// see MEMBERSHIP_EXPIRY_NO_DELAYED_MS in rust/src/rtc.rs, and change the two
/// together or a live participant starts vanishing mid-sentence.
constexpr qint64 kMembershipRepublishIntervalMs = 60 * 1000;
/// A retraction the server did not accept is retried this many times, with
/// the delay doubling. Bounded: leaving must not turn into an unbounded
/// background sender, and a server that keeps refusing will not start.
constexpr int kMaxRetractAttempts = 4;
constexpr int kRetractRetryDelayMs = 2000;
/// Presentation bound on the participant list.
constexpr int kMaxParticipants = 64;
} // namespace

SfuCallController::SfuCallController(QObject *parent) : QObject(parent)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    m_videoRouter = new SfuVideoRouter(this);
#endif
    // Created once, for the controller's whole life. A view binds to these
    // pointers and must never see them change: replacing the object per call
    // would be a rebind, which is the reset this whole layer exists to
    // avoid. Leaving empties them instead.
    m_participantModel = new CallParticipantModel(this);
    // participantCount() reads the model, so the property's NOTIFY has to be
    // the model's own signal. Every other route (emitting participantsChanged
    // from each rebuild site) is a list somebody has to keep complete, and it
    // was already incomplete: onSfuJoined and the mediaStateChanged rebuild
    // both moved the count without saying so.
    connect(m_participantModel, &CallParticipantModel::countChanged, this,
            &SfuCallController::participantCountChanged);
    m_shareModel = new CallShareModel(this);
    m_stageState = new CallStageState(this);
    m_stageState->setShareModel(m_shareModel);
    // OUR OWN media state is part of the local row and of the local share
    // row, and it changes from a dozen places (mute, deafen, camera, share
    // start/stop, teardown). Hooking the one signal they all already emit is
    // why none of them can forget: adding a rebuildModels() call to each
    // mutator would work until the next mutator is written.
    connect(this, &SfuCallController::mediaStateChanged, this,
            [this] { rebuildModels(); });
    m_refreshTimer.setInterval(kRefreshIntervalMs);
    connect(&m_refreshTimer, &QTimer::timeout, this,
            &SfuCallController::refreshMembership);
    m_retractRetryTimer.setSingleShot(true);
    connect(&m_retractRetryTimer, &QTimer::timeout, this,
            &SfuCallController::retryRetraction);
}

SfuCallController::~SfuCallController()
{
    // Never leave a microphone live because an object went away, and never
    // leave a membership behind because one did.
    //
    // WHAT THIS CAN AND CANNOT DO, plainly, because the difference is the
    // whole of the maintainer's report:
    //
    //  * A GRACEFUL exit reaches this and the retraction is dispatched. It can
    //    still complete after this object is gone, because the Rust bridge
    //    joins its room-action task pool with a budget during its own
    //    shutdown. That is a real dependency on another component's teardown
    //    order, and it is why AppController::prepareForShutdown() should call
    //    leave() explicitly rather than relying on member destruction order.
    //  * A KILLED process — SIGKILL, a segfault, an OOM kill, power loss, a
    //    lost network — runs NONE of this and sends nothing. No amount of
    //    code here can change that. The MSC4140 delayed retraction is the
    //    ONLY mechanism that survives it, which is exactly why the publish
    //    path treats "no delayed event armed" as a condition to be reported
    //    and compensated for (a short `expires` plus a re-publish cadence)
    //    rather than as a detail.
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
    // AND SO DID ANY OUTSTANDING RETRACTION. We were disconnected from the
    // old client above, so its answer can never arrive — but op ids come from
    // each client's OWN counter, so leaving the id set would let an unrelated
    // op from the NEW client be mistaken for the old retraction's answer. The
    // membership the old account left behind is now the old account's server
    // to clean up; nothing here can reach it any more, and pretending
    // otherwise is worse than saying so.
    // Warned only when a retry was ALREADY ARMED, i.e. an attempt had failed.
    // The retraction teardown just dispatched is still on its way through the
    // old client's bridge and will probably land; a warning there would cry
    // wolf on every account switch made during a call.
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
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::rtcMembershipPublished, this,
            &SfuCallController::onMembershipPublished);
    // NOTHING was connected to this before. `grep -rn rtcMembershipRetracted
    // src/` returned the declaration and the emit and nothing else — so a
    // retraction that failed (offline at hang-up, a 5xx, a rate limit) was
    // silent, unlogged and never retried, and the membership it failed to
    // remove sat in the room poisoning the call for every other client.
    connect(m_client, &MatrixClient::rtcMembershipRetracted, this,
            &SfuCallController::onMembershipRetracted);
    connect(m_client, &MatrixClient::sfuStateChanged, this,
            &SfuCallController::onSfuState);
    connect(m_client, &MatrixClient::sfuJoined, this,
            &SfuCallController::onSfuJoined);
    connect(m_client, &MatrixClient::sfuParticipantsChanged, this,
            &SfuCallController::onSfuParticipants);
    connect(m_client, &MatrixClient::sfuSpeakersChanged, this,
            &SfuCallController::onSfuSpeakers);
    // The bridge has emitted this since the interop round and NOBODY was
    // listening, so `connectionQuality` could only ever have been unknown.
    // It is a closed enum ("poor"/"good"/"excellent"/"unknown"), not
    // content, and it is the honest source for a per-tile quality badge.
    connect(m_client, &MatrixClient::sfuConnectionQuality, this,
            &SfuCallController::onSfuConnectionQuality);
    connect(m_client, &MatrixClient::sfuRemoteDescription, this,
            &SfuCallController::onSfuRemoteDescription);
    connect(m_client, &MatrixClient::sfuRemoteCandidate, this,
            &SfuCallController::onSfuRemoteCandidate);
    connect(m_client, &MatrixClient::rtcMediaKeyReceived, this,
            &SfuCallController::onMediaKeyReceived);
    // Raised hands, in element-call's own wire format. Three lanes: our own
    // send's answer, live changes from the sync loop, and the one join-time
    // sweep for hands raised before we arrived.
    connect(m_client, &MatrixClient::rtcHandResult, this,
            &SfuCallController::onHandResult);
    connect(m_client, &MatrixClient::rtcHandChanged, this,
            &SfuCallController::onHandChanged);
    connect(m_client, &MatrixClient::rtcHandsReceived, this,
            &SfuCallController::onHandsReceived);
    // The key SEND result was reported by the bridge and listened to by
    // NOBODY, so a distribution that reached zero devices was
    // indistinguishable from one that worked — and the only visible effect
    // was every remote frame being dropped for want of a key, which looks
    // like a dead call rather than a failed key. Counts only; never the key.
    connect(m_client, &MatrixClient::rtcMediaKeySent, this,
            [this](quint64, bool ok, const QString &category, int delivered,
                   int keyIndex) {
                if (ok) {
                    qCInfo(lcSfuCall) << "media key sent index=" << keyIndex
                                      << "delivered=" << delivered;
                    return;
                }
                qCWarning(lcSfuCall)
                    << "media key NOT sent index=" << keyIndex
                    << "category=" << category << "delivered=" << delivered;
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
    // THE MEMBERSHIP IS WHAT MAKES A KEY ADDRESSABLE, and it arrives on its
    // own schedule.
    //
    // A media key is sent to (user, device) pairs taken from the room's
    // MatrixRTC membership; the SFU's participant list is a different feed
    // over a different transport, and nothing orders the two. So a peer
    // routinely appears in the SFU list before their `m.call.member` state
    // event has been read — and rotateAndDistributeKey(), which runs on
    // exactly that SFU change, then finds NO targets and sends nothing.
    // Nothing re-sent it either, so both sides encrypted happily and dropped
    // every frame the other sent with "no key" for the whole call. That is
    // audio, video and screen share all dead at once while the call looks
    // perfectly connected.
    //
    // Re-running distribution when the membership lands closes the race from
    // the other side. It is safe to run repeatedly: distributeKeyIfNeeded()
    // only acts when the addressable set actually grew.
    connect(m_rtc, &RtcController::sessionChanged, this,
            [this](const QString &roomId) {
                if (!active() || roomId != m_roomId)
                    return;
                // BIND FIRST, then distribute.
                //
                // A frame names its sender only by the LiveKit sid, and a key
                // is stored under the sending DEVICE — so the two are one ring
                // only after noteParticipantIdentities() has joined them, and
                // that join needs the membership, which is what just arrived.
                //
                // Binding only from onSfuParticipants() was not enough: the
                // SFU announces a participant BEFORE their membership is read,
                // so the bind attempted there resolves nothing, and if the SFU
                // then sends no further update the binding never happens at
                // all. The key sits in a ring keyed by device while every
                // arriving frame consults the ring keyed by sid, and the
                // symptom is `decrypt failed` on every single frame — a key
                // that was received, installed, and never findable.
                noteParticipantIdentities();
                distributeKeyIfNeeded();
            });
}

void SfuCallController::setMediaEngine(SfuMediaEngine *engine)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    // The ASSIGNMENT lives inside the guard, not just the connects.
    // Assigning a QPointer<T> instantiates its static_cast to QObject*,
    // which needs T COMPLETE — and in a non-WebRTC build SfuMediaEngine is
    // only forward-declared. Same shape as the Qt 6.8 failure that broke
    // the 0.7.4 deb build; a member QPointer of an incomplete type is fine,
    // touching it is not. Without an engine there is nothing to set anyway.
    m_engine = engine;
    if (!m_engine)
        return;
    // Received frames need a destination before the first call, not after.
    m_engine->setVideoRouter(m_videoRouter);
    connect(m_engine, &SfuMediaEngine::localDescription, this,
            &SfuCallController::onEngineLocalDescription);
    connect(m_engine, &SfuMediaEngine::localCandidate, this,
            &SfuCallController::onEngineLocalCandidate);
    connect(m_engine, &SfuMediaEngine::failed, this,
            &SfuCallController::onEngineFailed);
    // NOT the same signal and NOT the same consequence: onEngineFailed ends
    // the call, and one broken capture device must not do that.
    connect(m_engine, &SfuMediaEngine::publishFailed, this,
            &SfuCallController::onEnginePublishFailed);
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
    // A volume can be changed from somewhere that is not this controller —
    // another surface, another window, a settings page — and the engine has
    // to hear about it or the slider and the sound disagree.
    connect(m_settings, &SettingsManager::callParticipantVolumeChanged, this,
            [this](const QString &, int) { applyStoredVolumes(); });
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
                // The portal granted exactly what the user picked. Guard on
                // still being in a call: the picker is modal to the desktop,
                // not to us, so the call can end while it is open.
                //
                // The fd is OURS now (the portal duplicated it for us), so
                // every path out of here closes it — including the refusals,
                // or a declined share leaks a descriptor per attempt.
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
        // The user declined. Deliberately silent: a dialog saying "you
        // cancelled" is noise.
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

SfuCallController::LinuxShareRoute SfuCallController::linuxShareRoute(
    bool portalAvailable, const QString &platformName,
    const QString &sessionType, const QString &waylandDisplay,
    const QString &x11Display, bool captureElementPresent)
{
    // THE PORTAL FIRST, BEFORE EVERY OTHER CLAUSE, and this ordering is the
    // contract rather than an optimisation. It is what makes sharing safe on
    // Wayland, it is what a normal KDE or GNOME session uses, and it draws a
    // picker with previews Lightning cannot draw itself. Everything below is
    // for a machine that does not have it.
    if (portalAvailable)
        return LinuxShareRoute::Portal;

    // WAYLAND REFUSES, AND IT REFUSES BEFORE THE X11 CLAUSES ARE REACHED.
    //
    // Not conservatism: there is genuinely no way to capture a Wayland
    // desktop without the portal, which is the entire reason the portal
    // exists. The trap is XWayland — a Wayland session hands an app a working
    // `DISPLAY`, so every X11 clause below would pass and produce a pipeline
    // that plays perfectly and sends a black rectangle (measured on this
    // repo's own KDE session: XWayland's root is the full 7680x2160 desktop
    // extent and 99.999% of its pixels are zero, because native Wayland
    // windows never touch it). `WAYLAND_DISPLAY` is what tells the two apart
    // when the platform plugin cannot, so ANY of the three signals is enough.
    if (platformName.startsWith(QLatin1String("wayland"), Qt::CaseInsensitive)
        || sessionType.compare(QLatin1String("wayland"), Qt::CaseInsensitive)
            == 0
        || !waylandDisplay.isEmpty()) {
        return LinuxShareRoute::RefuseWaylandNeedsPortal;
    }

    if (x11Display.isEmpty())
        return LinuxShareRoute::RefuseNoDisplayServer;
    // ASKED OF THE RUNNING REGISTRY, never assumed from the platform.
    // `ximagesrc` ships in gst-plugins-good and is very likely present
    // wherever the engine is — but "very likely" is how a share reports
    // success and carries nothing. A missing element is a pipeline that dies
    // at PLAYING, long after the user has been shown a picker and chosen.
    if (!captureElementPresent)
        return LinuxShareRoute::RefuseNoCaptureElement;
    return LinuxShareRoute::FallbackDisplays;
}

QString SfuCallController::linuxShareRefusal(LinuxShareRoute route)
{
    switch (route) {
    case LinuxShareRoute::Portal:
    case LinuxShareRoute::FallbackDisplays:
        return {};
    case LinuxShareRoute::RefuseWaylandNeedsPortal:
        // NAMES THE CAUSE AND THE FIX, which the old wording did neither of.
        // "Screen sharing isn't available on this desktop" tells a person on
        // KDE with a broken portal install nothing they can act on, and it is
        // the one refusal here whose remedy is a single package.
        return tr("Screen sharing on Wayland needs xdg-desktop-portal, and "
                  "it isn't responding. Install or start the portal for your "
                  "desktop — for example xdg-desktop-portal-kde or "
                  "xdg-desktop-portal-gnome — then try again.");
    case LinuxShareRoute::RefuseNoCaptureElement:
        // The element is NAMED FROM ITS ONE DEFINITION, never spelled here.
        // A refusal that names a different element from the one the probe
        // asked about, or the pipeline uses, sends the user to install the
        // wrong thing — and it is the shape of the missing-sctp defect, where
        // a probe list and a pipeline disagreed about what was needed.
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
    // NO ARITHMETIC HERE, DELIBERATELY. The rectangle must already be in
    // native, root-relative pixels; see nativeScreenRect() for why nothing
    // may be computed from QScreen::geometry() and devicePixelRatio(). All
    // this does is refuse shapes ximagesrc cannot be given.
    //
    // An X11 root rectangle cannot have a negative corner, and ximagesrc's
    // coordinate properties are UNSIGNED — so a negative origin would not
    // fail, it would wrap and capture somewhere else entirely. Refusing is
    // the only honest answer.
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
    // THE PLATFORM'S OWN RECTANGLE. See the header for the two independent
    // reasons QScreen::geometry() and devicePixelRatio() cannot produce one,
    // both measured on a real two-monitor 4K desktop.
    //
    // `handle()` is null while a screen is being torn down, which is a real
    // state during a monitor hot-unplug — exactly when this is most likely to
    // be asked. Answering "no rectangle" then is correct: the caller refuses
    // rather than capturing a guess.
    const QPlatformScreen *platform = screen->handle();
    if (!platform)
        return {};
    return validX11CaptureRect(platform->geometry());
#else
    // Windows and macOS address a display by INDEX through their own capture
    // elements and never need a root rectangle, so this has no caller there
    // and no private Qt header is compiled into those builds. On a Linux Qt
    // built without private headers there is no sound way to obtain one, and
    // an unsound one shares the wrong screen — so the fallback lists no
    // display and refuses, which is the honest outcome.
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
    // DISPLAYS ONLY, AND THAT IS A DECISION RATHER THAN A GAP.
    //
    // `ximagesrc` does take an `xid`, so a window list is reachable in
    // principle — and it is refused for the same reason
    // `WindowCaptureSrc.h` refuses to crop the screen to a window's
    // rectangle. Without a compositing manager, reading a window's drawable
    // returns whatever is STACKED ON TOP of it: another app, a password
    // prompt, a notification. With one, whether the redirected pixmap is
    // readable at all depends on the compositor. Neither outcome can be
    // verified from this machine, and a share that may leak a window the
    // user did not choose is not a feature. A display list that works is
    // worth more than a window list that half does.
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
        // THE NATIVE rectangle, which is both what the capture will take and
        // the only honest thing to put on the row: `QScreen::geometry()`
        // reports a device-INDEPENDENT size (2560x1440 for a 3840x2160
        // panel on the measured desktop), and no arithmetic recovers the
        // real one — see nativeScreenRect().
        //
        // A screen whose native rectangle cannot be had is SKIPPED rather
        // than offered with a guessed one. An unlisted display is a missing
        // choice; a listed one that captures the wrong region is a share the
        // user did not consent to.
        const QRect rect = nativeScreenRect(screen);
        if (!rect.isValid())
            continue;
        // The SAME row shape the Windows picker builds, deliberately: one
        // picker, one contract. No `windowHandle` key at all, which is how
        // `ScreenSharePicker.qml` classifies a row as a display.
        m_screenShareSources.append(QVariantMap{
            { QStringLiteral("index"), i },
            // The platform's own name for the output ("DP-1", "HDMI-A-1"),
            // which is what the desktop's own display settings call it.
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
    // Windows and macOS have no portal to reach, so `available()` answers
    // "can this platform name a screen at all" and a false here is a machine
    // with no display. Unchanged.
    if (!portalUsable) {
        qCWarning(lcSfuCall) << "screen share unavailable portal="
                             << !m_portal.isNull();
        Q_EMIT callFailed(
            tr("Screen sharing isn't available on this desktop."));
        return;
    }
#endif
    // Already sharing? STOP the old share first rather than opening a second
    // portal session beside it. Two live sessions leave one orphaned — the
    // compositor keeps capturing for a pipeline nothing reads — and the
    // second request is refused as `busy` anyway, which is what made the
    // button look broken after the first share.
    if (m_screenSharing)
        stopScreenShare();
    if (!m_portal.isNull() && m_portal->busy()) {
        qCInfo(lcSfuCall) << "screen share already being chosen; ignoring";
        return;
    }
    qCInfo(lcSfuCall) << "screen share requested";
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // NO PORTAL, SO LIGHTNING OWNS THE PICKER HERE.
    //
    // The capture element takes a display index and nothing asks the user
    // which one. Until this existed a share silently took whichever display
    // the app happened to be on — reported as "no option to select which
    // window or what screen, it just grabs full monitor".
    //
    // Windows/displays only, and honestly so: `gdiscreencapsrc` captures a
    // MONITOR (its properties are `monitor` and a crop rectangle) and
    // `avfvideosrc capture-screen` captures a DISPLAY. Single-window capture
    // needs elements this toolchain cannot ship (see
    // docs/windows-packaging.md), so the picker offers what the pipeline can
    // actually deliver rather than a window list that would fail on use.
    m_screenShareSources.clear();
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QWindow *ownWindow = QGuiApplication::focusWindow();
    const QScreen *ownScreen = ownWindow ? ownWindow->screen() : nullptr;
    if (!ownScreen)
        ownScreen = QGuiApplication::primaryScreen();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        // PHYSICAL pixels, which is what gets captured. `QScreen::geometry()`
        // is device-INDEPENDENT, so at the 125% scaling this was reported on
        // a 3840x2160 display was offered to the user as "3072 x 1728" — the
        // "it didnt show 4k on resolution" report, and a number that appears
        // nowhere else on the machine. The capture element takes a monitor
        // index and grabs the real framebuffer; the row has to agree with it.
        QSize g = screen->geometry().size() * screen->devicePixelRatio();
        // AND THE INDEX THE CAPTURE ACTUALLY COUNTS IN.
        //
        // Qt's screen order, the order EnumDisplayMonitors walks, and
        // whatever `gdiscreencapsrc`'s `monitor` property counts are three
        // enumerations with nothing mapping between them. Passing Qt's index
        // to the capture was a guess that happens to hold on most machines
        // and, when it does not, names one display, previews a second and
        // captures a third. Resolved by DEVICE NAME instead, which is exact
        // — and which also yields the monitor's real framebuffer rectangle,
        // better than any arithmetic on a device-independent geometry.
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
            // The platform's own name for the display, so the row matches
            // what the OS display settings call it.
            { QStringLiteral("name"), screen->name() },
            { QStringLiteral("application"), QString() },
            { QStringLiteral("geometry"),
              QStringLiteral("%1 x %2").arg(g.width()).arg(g.height()) },
            { QStringLiteral("primary"),
              screen == QGuiApplication::primaryScreen() },
            { QStringLiteral("current"), screen == ownScreen },
        });
    }
    // ...AND THE WINDOWS, which is the half a person actually reaches for.
    //
    // Every other call client offers applications, and the reason this one
    // did not was never design: no GStreamer element we can ship captures a
    // window, so Lightning brings its own (WindowCaptureSrc.h). Empty off
    // Windows, where the portal owns the picker and already offers windows
    // itself.
    for (const lightning::wincap::WindowInfo &window :
         lightning::wincap::enumerateWindows()) {
        m_screenShareSources.append(QVariantMap{
            { QStringLiteral("index"), -1 },
            { QStringLiteral("windowHandle"), window.handle },
            { QStringLiteral("name"), window.title },
            // WHICH APPLICATION, separately from the caption. A Chromium
            // window's caption is the TAB's title and names no browser at
            // all, so a picker offering the caption alone leaves the user
            // guessing what they are about to broadcast.
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
    // ONE source and it is a DISPLAY: not a choice, so share it. Opening a
    // dialog to confirm the only possible answer is a click that tells the
    // user nothing.
    //
    // Windows can no longer take this path in practice — a desktop with a
    // window open has at least two entries — and that is the point: the
    // report was "it just grabs the main monitor", and a single-monitor
    // machine used to hit exactly this branch and share without asking.
    if (m_screenShareSources.size() == 1) {
        chooseScreenShareSource(0);
        return;
    }
    Q_EMIT screenShareSourcesAvailable();
#else
    // ONE DECISION, TAKEN BY THE PURE PREDICATE, so the ordering that matters
    // — portal first, Wayland refused before any X11 clause — is the same
    // thing a test can hold to account. Nothing below re-derives it.
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
        // Monitors and windows. Virtual sources are deliberately not offered:
        // they exist for remote-desktop use and would confuse the picker
        // here. The portal draws the dialog, so nothing is enumerated here.
        m_portal->requestShare(ScreenCastPortal::Monitor
                               | ScreenCastPortal::Window);
        return;
    case LinuxShareRoute::FallbackDisplays:
        // NO PORTAL ON AN X11 SESSION, so Lightning draws the SAME picker
        // Windows and macOS get. Before this there was no way to share at all
        // and no way to choose.
        if (!populateLinuxDisplaySources()) {
            Q_EMIT callFailed(tr("No display is available to share."));
            return;
        }
        Q_EMIT screenShareSourcesChanged();
        // ONE display is not a choice — the user asked to share a screen and
        // there is exactly one. Same rule as the Windows branch, and the same
        // reason a dialog offering a single answer is a click that tells
        // nobody anything.
        if (m_screenShareSources.size() == 1) {
            chooseScreenShareSource(0);
            return;
        }
        Q_EMIT screenShareSourcesAvailable();
        return;
    case LinuxShareRoute::RefuseWaylandNeedsPortal:
    case LinuxShareRoute::RefuseNoCaptureElement:
    case LinuxShareRoute::RefuseNoDisplayServer:
        // REFUSED WITH THE REASON, and no picker. Offering a dialog here
        // would be offering a capture that cannot exist — on Wayland
        // especially, where an X11 fallback would run flawlessly and send a
        // black rectangle.
        Q_EMIT callFailed(linuxShareRefusal(route));
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
    // Read BEFORE the list is cleared. The chosen row carries which kind it
    // is, and clearing first would leave only the index — which says nothing
    // about whether it means a display or a window.
    const QVariantMap chosen = m_screenShareSources.at(index).toMap();
    const quint64 windowHandle =
        chosen.value(QStringLiteral("windowHandle")).toULongLong();
    const int displayIndex = chosen.value(QStringLiteral("index")).toInt();
    const bool isWindow = windowHandle != 0;

    // ON THE LINUX FALLBACK THE CAPTURE IS A RECTANGLE, not an index, because
    // `ximagesrc` addresses the X11 ROOT WINDOW and a monitor is a region of
    // it. Resolved from the row's screen NAME and resolved NOW rather than
    // remembered from when the list was built: a monitor unplugged while the
    // dialog was open renumbers every display after it, and sharing the
    // wrong screen is exactly the failure this picker exists to prevent.
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
    // THE THREE SOURCE KINDS, exactly one of which is meaningful per share.
    // A display index goes in the node-id slot, which is what
    // SfuMediaEngine::screenShareSource() reads it as on Windows and macOS.
    // No portal remote exists on any of these paths, hence -1 for the fd. A
    // window carries its handle instead, and the Linux fallback carries its
    // root rectangle; in each of those two the node id means nothing.
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
    // NOTHING LEAVES THIS PROCESS. No membership is published, no SFU is
    // contacted, no capture device is opened. The only things written are the
    // controller's own presentation state and the participant model the call
    // surface reads — which is exactly what a screenshot needs and nothing
    // more. See the header for why this is compiled out of release builds.
    if (roomId.isEmpty())
        return;
    m_roomId = roomId;
    // Before any emit below: `mediaStateChanged` runs `rebuildModels()`
    // synchronously, and that would reconcile these people out of existence.
    m_demoCall = true;

    struct DemoPerson {
        const char *id;
        const char *name;
        bool local;
        bool micMuted;
        bool camera;
        bool hand;
    };
    // Fictional, and deliberately so: these names exist nowhere but the demo
    // seed. A screenshot must never carry a real account.
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
        // KNOWN, so the tiles draw real state rather than the unknown
        // placeholder — the whole point is to photograph the populated case.
        row.micKnown = true;
        row.micMuted = p.micMuted;
        row.cameraKnown = true;
        row.cameraOn = p.camera;
        if (p.camera)
            row.cameraTrackKey = QStringLiteral("TR_demo_cam_") + row.sid;
        rows.append(row);
    }
    if (withScreenShare) {
        // The share rides on a REAL participant, as it does in a live call —
        // a share with no sharer would be a shape the model cannot produce.
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
        // One speaker, so the speaking ring is in the picture.
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
    m_state = state;
    m_lastError = error;
    Q_EMIT stateChanged();
}

QString SfuCallController::userFacingError(const QString &category) const
{
    // A closed set in, plain wording out. A raw category or a server string
    // must never reach the user (§47 of the calling brief, and the repo's
    // standing rule about rendering remote text).
    if (category == QLatin1String("forbidden"))
        return tr("You don't have permission to join this call.");
    if (category == QLatin1String("unsupported"))
        return tr("Calling isn't available on this homeserver.");
    if (category == QLatin1String("rate_limited"))
        return tr("Too many attempts. Try again in a moment.");
    if (category == QLatin1String("network")
        || category == QLatin1String("connect_failed")
        || category == QLatin1String("connection_lost"))
        return tr("Couldn't connect to the call.");
    if (category == QLatin1String("server_error"))
        return tr("The calling service is having trouble.");
    // Before the startsWith below, which would otherwise tell someone whose
    // shared window they just closed that it "couldn't start".
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

    // THE safety gate. An encrypted room whose media cannot be encrypted is
    // refused outright rather than joined in the clear: the user was told
    // that room is end-to-end encrypted, and carrying their audio where the
    // SFU can read it would make that untrue.
    const QString block = m_rtc->joinBlockReason(roomId);
    if (!block.isEmpty())
        qCWarning(lcSfuCall) << "join refused: block=" << block;
    if (block == QLatin1String("media_encryption_unavailable")) {
        setState(State::Failed,
                 tr("This room is encrypted, and encrypted calls aren't "
                    "available yet on this build."));
        Q_EMIT callFailed(m_lastError);
        return false;
    }

    // One call at a time, globally: tear the previous one down explicitly
    // rather than leaving a second engine holding the microphone.
    if (active())
        teardown(State::Ended);

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
    m_participants.clear();
    m_speaking.clear();
    m_speakingLevel.clear();
    m_connectionQuality.clear();
    // A new call starts with an empty stage. The models are emptied rather
    // than replaced so a view bound to them stays bound.
    if (m_participantModel)
        m_participantModel->clear();
    if (m_shareModel)
        m_shareModel->clear();
    if (m_stageState)
        m_stageState->clear();
    m_publishedTrackIds.clear();
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_membershipEventId.clear();
    m_delayId.clear();
    m_ownIdentity.clear();
    m_mediaEncrypted = false;
    m_keyIndex = 0;
    m_candidatesSent = 0;
    m_lastKeyTargets.clear();
    m_lastPublishMs = 0;
    m_refreshOp = 0;
    m_delayedRestartOp = 0;
    // A retraction still being retried for THIS room is now moot — we are
    // re-joining it, and the publish below is what the room should end up
    // with. Letting the retry keep firing would remove the membership we are
    // about to create. (An attempt already IN FLIGHT cannot be recalled; its
    // answer is ignored because m_retractOp is cleared, but a reply race
    // remains possible and is why the refresh cadence exists.)
    if (m_retractRoomId == roomId) {
        m_retractRetryTimer.stop();
        m_retractRoomId.clear();
        m_retractDelayId.clear();
        m_retractAttempts = 0;
        m_retractOp = 0;
    }

    // Captured once for the call, so a room-state change mid-call cannot
    // quietly relax what we already promised the user. Unknown is true.
    m_roomEncrypted = m_rtc->roomEncrypted(roomId);
    // Armed BEFORE any media exists. With this set the pad probes DROP a
    // frame they have no key for, which is what makes the promise real
    // rather than a label.
    m_engine->setEncryptionRequired(m_roomEncrypted);
    m_engine->clearKeys();

    // The focus other participants advertise, or the homeserver's own.
    // Empty is legal: the server may name one we simply do not echo.
    m_focusUrl = m_rtc->focusUrlFor(roomId);

    setState(State::Preparing);
    Q_EMIT mediaStateChanged();
    Q_EMIT participantsChanged();

    // Membership FIRST, carrying the focus. Other clients pick their SFU
    // from the oldest membership, so ours has to be on the wire before we
    // expect anyone to meet us there.
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

void SfuCallController::onMembershipPublished(quint64 opId, bool ok,
                                              const QString &category,
                                              const QString &eventId,
                                              const QString &delayId)
{
    if (opId == 0)
        return;
    // A REFRESH re-publish, not the join's first one. It must not re-run the
    // join sequence — but it MUST adopt the delay id, because a re-publish
    // arms a brand new delayed retraction and the old id is dead.
    if (m_refreshOp != 0 && opId == m_refreshOp) {
        m_refreshOp = 0;
        if (!active())
            return;
        if (!ok) {
            // Not fatal and not repaired here: the next heartbeat tick tries
            // again, and the published `expires` is sized to survive several
            // consecutive failures. Logged because a membership that stops
            // refreshing is how a live participant silently disappears.
            qCWarning(lcSfuCall)
                << "membership refresh FAILED category=" << category;
            return;
        }
        m_delayId = delayId;
        qCInfo(lcSfuCall) << "membership refreshed delayed="
                          << !delayId.isEmpty();
        return;
    }
    if (opId != m_publishOp)
        return;
    m_publishOp = 0;
    qCInfo(lcSfuCall) << "membership published ok=" << ok
                      << "category=" << category
                      << "delayed=" << !delayId.isEmpty();
    if (m_state != State::Preparing)
        return; // a reply for a call we already left
    if (!ok) {
        teardown(State::Failed, userFacingError(category));
        Q_EMIT callFailed(m_lastError);
        return;
    }
    m_membershipEventId = eventId;
    // Empty means the server has no MSC4140. Cleanup then relies ENTIRELY on
    // the membership's own `expires`, which the Rust side shortens to minutes
    // for exactly that case — and which only works because refreshMembership()
    // now re-publishes on a cadence. Both halves are required; either one
    // alone is worse than what was here before.
    m_delayId = delayId;
    if (delayId.isEmpty()) {
        qCWarning(lcSfuCall)
            << "no MSC4140 delayed retraction armed — an unclean exit will "
               "leave this membership until it expires";
    }
    m_lastPublishMs = QDateTime::currentMSecsSinceEpoch();
    m_refreshTimer.start();

    // The hands already up when we arrived. Spent ONCE per join: a hand
    // raised before this client joined produces no sync event for us, so
    // without this pass an early raiser is invisible for the whole call.
    // After this the sync handler carries every change and costs nothing.
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
        teardown(State::Failed, userFacingError(category));
        Q_EMIT callFailed(m_lastError);
        return;
    }
    if (state == QLatin1String("ended") || state == QLatin1String("closed")) {
        // The SFU dropped us. Not a user action, so it is reported rather
        // than silently becoming Ended.
        if (m_state == State::Connected || m_state == State::Connecting) {
            teardown(State::Failed, tr("The call ended because the "
                                       "connection was lost."));
            Q_EMIT callFailed(m_lastError);
        }
    }
}

void SfuCallController::onSfuJoined(const QString &identity,
                                     const QVariantList &participants,
                                     const QVariantList &iceServers)
{
    qCInfo(lcSfuCall) << "sfu joined others=" << participants.size()
                      << "iceServers=" << iceServers.size()
                      << "identity=" << (identity.isEmpty()
                                         ? QStringLiteral("<empty>")
                                         : QStringLiteral("<set>"))
                      << "active=" << active();
    if (!active())
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    m_ownIdentity = identity;
    m_participants = participants.mid(0, kMaxParticipants);
    noteParticipantIdentities();
    rebuildModels();
    if (!m_engine.isNull()) {
        m_engine->start();
        m_engine->setIceServers(iceServers);
        applyAudioState();
        publishTracks();
    }
    setState(State::Connecting);
    Q_EMIT participantsChanged();
    // The key is minted inside publishTracks(), before the first frame can
    // be encrypted — not here, or we would distribute two in a row.
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
    // The key BEFORE the first frame. A probe with no key drops in an
    // encrypted room, so publishing first would mean our own audio is
    // silently discarded until the key lands.
    if (m_roomEncrypted)
        rotateAndDistributeKey();
    // The client chooses the track id and DECLARES it before negotiating, so
    // the SFU can map the negotiated media section to the track it
    // authorized. Declaring and publishing must use the same id.
    const QString audioCid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_client->sfuAddTrack(audioCid, QStringLiteral("microphone"), 0,
                          /*width=*/0, /*height=*/0, false, m_roomEncrypted);
    m_engine->publishAudio(audioCid);
    m_audioCid = audioCid;
    m_publishedTrackIds.append(audioCid);

    if (m_cameraOn) {
        const QString videoCid =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        // The ceiling the camera pipeline scales to. Declaring the real
        // shape is what stops the SFU inferring simulcast (SfuMediaEngine's
        // caps are the authority on these numbers).
        m_client->sfuAddTrack(videoCid, QStringLiteral("camera"), 1,
                              SfuMediaEngine::kCameraWidth,
                              SfuMediaEngine::kCameraHeight,
                              false, m_roomEncrypted);
        m_engine->publishVideo(videoCid, /*screenShare=*/false,
                               /*nodeId=*/-1);
        m_cameraCid = videoCid;
        m_publishedTrackIds.append(videoCid);
    }
#endif
}

void SfuCallController::onSfuParticipants(const QVariantList &updates)
{
    if (!active())
        return;
    const bool setChanged = mergeParticipants(updates);
    // A LEAVER must stop being able to decrypt, so any change in the set
    // rotates the key. Rotating on joins too is the simple, safe choice:
    // the alternative is tracking who is new, and being wrong about that
    // means someone keeps a key they should not have.
    if (setChanged)
        rotateAndDistributeKey();
    // The track sid arrives with this update, so a mute the user made before
    // the SFU announced the track is applied here — and a state that drifted
    // for any other reason is corrected on the next update rather than
    // staying wrong for the rest of the call.
    //
    // ALL THREE sources, not just the microphone. A camera or a share that
    // was stopped before the SFU had named its track would otherwise never be
    // reconciled at all, and this is the retry that makes a stop that raced
    // the announcement converge instead of leaving a phantom track live for
    // the rest of the call.
    syncMicMuteToSfu();
    applyVideoState();
    // Ask for the room's membership. A participant the SFU has announced but
    // whose membership we have not read cannot be sent a key, and without
    // this the read happens only when the user opens the room or the server
    // happens to poke us.
    if (m_rtc && !m_roomId.isEmpty())
        m_rtc->refresh(m_roomId);
}

bool SfuCallController::mergeParticipants(const QVariantList &updates)
{
    // The identity SET, not the count. An update can legitimately carry a
    // join and a disconnect at once, which leaves the size unchanged — and
    // keying the key rotation on the size then lets a participant who LEFT
    // keep a key that still decrypts everything said after they went. The
    // count was only ever a proxy for this.
    QSet<QString> before;
    for (const QVariant &row : std::as_const(m_participants)) {
        before.insert(
            row.toMap().value(QStringLiteral("identity")).toString());
    }
    // A LiveKit `ParticipantUpdate` is a DELTA, not the room.
    //
    // It carries only the participants whose state changed — including OUR
    // OWN row, which is how the client learns the track sids the server
    // assigned. Assigning it over the list therefore threw away everyone it
    // did not mention: publishing our audio produced an update about us and
    // erased the person we were talking to, and their next update erased us.
    // That is the "1 person in call" report, and it took the media key with
    // it, because a key can only be installed against a participant we still
    // hold. livekit-client merges by identity and removes on DISCONNECTED
    // (`Room.handleParticipantUpdates`); so does this.
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
        // LiveKit's SpeakerInfo carries `level` (0..1, 1 is loudest) and
        // rust/src/sfu.rs has forwarded it all along; this used to read
        // `active` and throw the amplitude away, which is the single reason
        // a volume-reactive ring was impossible. ABSENT stays absent — the
        // model treats a missing level as 0.0 and draws its minimum ring
        // rather than inventing an amplitude from the boolean.
        if (entry.contains(QStringLiteral("level"))) {
            m_speakingLevel.insert(
                sid, entry.value(QStringLiteral("level")).toDouble());
        }
    }
    // PER-ROW dataChanged on the speaking roles only — never
    // participantsChanged(), which QML answered by rebuilding the whole
    // participant array. That rebuild was a model reset, and it destroyed
    // every tile and every VideoOutput in it on every syllable.
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
            continue; // unknown is not a value to render; it is the default
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

// The SDP itself is never logged: it carries host IPs. Only the fact and the
// direction, which is what "did we ever offer?" needs — a session that reaches
// LiveKit's 60s join timeout with JOIN_FAILURE never completed a peer
// connection, and the first question is whether an offer was even produced.
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
    // Counted, not printed: a candidate carries host IPs. Zero candidates on
    // the publisher is the signature of a peer connection that never got off
    // the ground.
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

void SfuCallController::onEngineFailed(const QString &category)
{
    qCWarning(lcSfuCall) << "engine failed category=" << category
                         << "active=" << active();
    if (!active())
        return;
    // A media failure ends the call: continuing would leave the user in a
    // session they cannot hear or be heard in, with no indication why.
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
    // TURN THE BUTTON BACK OFF. This is the whole point: the engine's bus
    // errors were logged and never raised, so a camera that could not
    // negotiate left `cameraOn` true, the control lit, and the far end with a
    // declared track carrying nothing — for the rest of the call, with the
    // reason only in a log. The call itself is fine and stays up.
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
        // A track we no longer own. Nothing to correct, and nothing to say:
        // the user has already moved on from it.
        return;
    }
    applyVideoState();
    Q_EMIT mediaStateChanged();
    // Reuses the existing user-facing notice, exactly as the portal-failure
    // path does — `callFailed` is "a failure, in plain wording", not "the
    // call ended"; the state is untouched here and `active()` stays true.
    Q_EMIT callFailed(userFacingError(category));
#else
    Q_UNUSED(cid);
    Q_UNUSED(category);
#endif
}

void SfuCallController::onMediaKeyReceived(const QString &roomId,
                                            const QString &sender,
                                            const QString &claimedDeviceId,
                                            int keyIndex,
                                            const QString &keyBase64)
{
    // Logged BEFORE every early return, because "the key never arrived" and
    // "the key arrived and we discarded it" are different faults with the
    // same symptom: every remote frame dropped for want of a key. Counts and
    // an index only — never the key, never the sender.
    qCInfo(lcSfuCall) << "media key received index=" << keyIndex
                      << "forThisRoom=" << (roomId == m_roomId)
                      << "active=" << active();
    if (!active() || roomId != m_roomId)
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_engine)
        return;
    if (keyIndex < 0 || keyIndex > 15)
        return;
    // Sender-chosen bytes. Bounded before decoding, then length-checked:
    // a key of the wrong size is not a key, and the cryptor refuses it
    // anyway — this only avoids doing the work.
    if (keyBase64.size() > 256)
        return;
    const QByteArray raw =
        QByteArray::fromBase64(keyBase64.toUtf8(),
                               QByteArray::AbortOnBase64DecodingErrors);
    // 16 OR 32 raw bytes. element-call mints 16 and livekit-client 32; both
    // derive the same AES-128 key through HKDF. Hardcoding 32 here dropped
    // every key an Element peer ever sent — see CallFrameCryptor's
    // isSupportedRawKeyLength(), which is the authority on this.
    if (raw.size() != 16 && raw.size() != 32) {
        qCWarning(lcSfuCall) << "media key refused: unsupported length"
                             << raw.size();
        return;
    }
    // Keys live ONLY in the engine's cryptor, which is the only thing that
    // needs them: never stored, never logged, never in QML.
    //
    // Stored under the SENDING DEVICE's own name, which is derivable from
    // the Olm-decrypted sender alone and is therefore ALWAYS available here.
    //
    // This used to resolve the LiveKit sid first and RETURN when it could
    // not. But a to-device key, the SFU's participant list and the room's
    // MatrixRTC membership arrive in no particular order, so a key that won
    // that race was discarded outright — and nothing re-sends it, so that
    // sender stayed undecryptable for the whole call. Now the key is always
    // kept, and the sid is bound to it whenever the participant list makes
    // that possible (noteParticipantIdentities, re-run on every update).
    const QString ringName = mediaKeyRingName(sender, claimedDeviceId);
    m_engine->setInboundKey(ringName, keyIndex, raw);
    // Alias the ring to the sender's SFU IDENTITY as well, so binding it to
    // an arriving frame never needs the room's membership to have resolved.
    //
    // `{user}:{device}` is not a guess: it is the identity the reference
    // treats as the default whenever a session membership omits
    // `membershipID` ("Other clients will treat undefined as
    // `${sender}:${device_id}`"), and it is what the JWT service assigns.
    // The membership-derived identity is aliased too where it is known,
    // which is what covers the sticky format's hashed identity.
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
    Q_UNUSED(keyIndex); Q_UNUSED(keyBase64);
#endif
}

void SfuCallController::attachVideoSink(const QString &identity,
                                        QObject *videoSink)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    // A QVideoSink arrives from QML as a QObject*; cast rather than trust.
    // A wrong type attaches nothing instead of being reinterpreted.
    auto *sink = qobject_cast<QVideoSink *>(videoSink);
    // The camera's own track key first, so a participant who is sharing a
    // screen AND a camera feeds two different surfaces. The participant sid
    // remains the fallback: it is what the engine routes under when the SFU
    // states no mid, and it is what worked before per-track routing existed.
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
    // NO participant-sid fallback here, deliberately. That key is where a
    // camera also lands, so using it for the screen surface would render a
    // face where the user asked for a screen — worse than rendering nothing.
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
    // A cast, not a trust. A wrong type releases NOTHING rather than being
    // reinterpreted — and, unlike the four key-named detaches this replaced,
    // a null argument here cannot clear anybody's route.
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
            // The track's SID, which is what the subscriber SDP's msid
            // carries and therefore what the engine routes under. NOT the
            // `mid`: LiveKit states a TrackInfo's mid on the PUBLISHER's
            // connection, while our subscriber transceiver gets its own,
            // independently assigned — so keying on it meant a remote screen
            // share arrived, decrypted, and had no surface waiting for it.
            const QString sid = track.value(QStringLiteral("sid")).toString();
            if (sid.isEmpty())
                continue;
            // PREFER A LIVE TRACK OVER A MUTED ONE.
            //
            // A stop is now expressed to the SFU as a mute rather than a
            // removal (applyVideoState), because this wire has no unpublish
            // verb — so one participant's row can carry the muted corpse of a
            // finished share AND the live one that replaced it. Returning the
            // first match would name the dead track, and the surface would
            // render nothing for a share that is plainly running. First
            // unmuted wins; the first match is kept as the fallback so a
            // participant whose only track is muted still resolves to
            // something addressable.
            if (!track.value(QStringLiteral("muted")).toBool())
                return sid;
            if (fallback.isEmpty())
                fallback = sid;
        }
        return fallback;
    }
    return {};
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
    // THE DEVICES ACTUALLY IN THE CALL — the SFU's participant list — and not
    // merely every device with a membership event in the room.
    //
    // A membership is a state event with a four-hour default expiry, so a
    // client that died without retracting leaves a GHOST behind: a device
    // that is not in the call, is not syncing, and cannot receive anything.
    // Addressing the key by membership alone sent it to exactly those ghosts
    // — measured against the real homeserver, the key sat undelivered in
    // Synapse's `device_inbox` for a dead device while the live peer got
    // nothing, so every frame it sent was dropped for want of a key. The call
    // connects, audio flows one way (the peer has OUR key), and the return
    // direction is silent forever.
    //
    // The SFU list is the authority on presence: a participant is there
    // because they hold an open signalling connection. The membership is
    // still what maps an SFU identity to a Matrix device, so both are needed
    // — this is the INTERSECTION, which is the only correct answer.
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
        // Our own device is never a target, and an identity the membership
        // has not resolved yet is SKIPPED rather than guessed — the next
        // participant update or membership read tries again.
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
    // A key ring belongs to one DEVICE. The same person on a laptop and a
    // phone are two senders publishing two independent key streams, and
    // LiveKit's key index is per-participant, so both legitimately use index
    // 0 with different material — collapsing them by user would decrypt one
    // with the other's key. The unit separator cannot occur in either half.
    if (userId.isEmpty() || deviceId.isEmpty())
        return {};
    return userId + QChar(0x1f) + deviceId;
}

void SfuCallController::noteParticipantIdentities()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_rtc)
        return;
    // The engine decrypts per SENDING DEVICE; a frame names its sender only
    // by the LiveKit sid in the subscriber SDP's `msid`; and a media key
    // names a Matrix device. This is the join between those two names.
    //
    // Re-run on every participant update AND on every key, because neither
    // the sid (which the SFU assigns) nor the membership (which resolves the
    // sid's identity to a device) is knowable at a fixed point — and a
    // binding that could not be made yet is retried simply by running this
    // again, with no key ever being buffered or re-requested.
    for (const QVariant &row : std::as_const(m_participants)) {
        const QVariantMap participant = row.toMap();
        const QString identity =
            participant.value(QStringLiteral("identity")).toString();
        const QString sid = participant.value(QStringLiteral("sid")).toString();
        if (identity.isEmpty() || sid.isEmpty())
            continue;
        // The sid to the IDENTITY first, and unconditionally. Both come
        // from the same SFU participant row, so this binding needs nothing
        // else to have arrived — which matters, because the arriving frames
        // name only the sid and every one of them is DROPPED until the ring
        // they land in is the ring the key went into. Making that depend on
        // the room's membership meant a membership that never resolved was
        // total, permanent silence with signalling working perfectly.
        m_engine->noteParticipantIdentity(sid, identity);
        // ...and to the sending device where the membership knows it, which
        // is what covers an identity that is NOT `{user}:{device}` — the
        // sticky format hashes it. Empty means "not resolved yet": never a
        // guess, because binding a sid to the wrong device would decrypt one
        // participant's frames with another's key. The next update retries.
        const QVariantMap person =
            m_rtc->participantForIdentity(m_roomId, identity);
        const QString name = mediaKeyRingName(
            person.value(QStringLiteral("userId")).toString(),
            person.value(QStringLiteral("deviceId")).toString());
        if (!name.isEmpty())
            m_engine->noteParticipantIdentity(sid, name);
    }
#endif
}

void SfuCallController::distributeKeyIfNeeded()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || !m_rtc || !m_roomEncrypted)
        return;
    // The set of devices we can currently address. Compared against what the
    // last distribution reached, so this is idempotent: a membership read
    // that reveals nobody new does nothing, and the sessionChanged signal can
    // therefore be connected without fear of a rotation storm.
    const QString targets = mediaKeyTargets();
    if (targets == m_lastKeyTargets)
        return;
    if (targets == QLatin1String("[]"))
        return;
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

    // 32 bytes from the system CSPRNG. QRandomGenerator::system() is
    // getrandom(2) here; the generic generator is a PRNG and must never be
    // used for key material.
    QByteArray key(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32 *>(key.data()),
        reinterpret_cast<quint32 *>(key.data() + key.size()));

    const int index = (m_keyIndex + 1) % 16;

    // Distribute FIRST, install second. The other way round means our own
    // frames are already encrypted under a key nobody else has yet, and
    // every receiver drops them until the to-device message lands.
    //
    // Sending to an empty target list is not an error and not a no-op we
    // should skip: a call we are alone in still encrypts, and a key nobody
    // needed yet is the correct state.
    const QString targets = mediaKeyTargets();
    // Remembered so distributeKeyIfNeeded() can tell "nobody new" from "a
    // peer we could not address last time is addressable now". An EMPTY set
    // is deliberately not remembered: it means the membership has not been
    // read yet, and recording it would make the retry a no-op forever.
    if (targets != QLatin1String("[]"))
        m_lastKeyTargets = targets;
    qCInfo(lcSfuCall) << "media key distributed index=" << index
                      << "targets=" << (targets == QLatin1String("[]")
                                        ? 0 : targets.count(QLatin1Char('{')));
    const quint64 op =
        m_client->rtcSendMediaKey(m_roomId, QString::fromUtf8(key.toBase64()),
                                  index, targets);
    Q_UNUSED(op);

    m_keyIndex = index;
    m_engine->setOutboundKey(index, key);
    // Best-effort scrub of our own transit copy. §16 is explicit that this
    // is hygiene, not a guarantee: the base64 QString handed to the bridge
    // is copied and dropped without zeroing.
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
        // MSC4140 is doing the work: keep the server's own retraction from
        // firing. The membership state event does NOT need re-publishing on
        // this path — its `expires` is the ecosystem's four hours precisely
        // because the delayed retraction is what cleans up after a crash, and
        // re-writing a state event while a delayed event is armed for the
        // same state key is behaviour nobody here has measured.
        //
        // The op id is REMEMBERED so a failure is actionable: a 404 means the
        // server already fired the retraction and we are gone from every
        // other client's list while still publishing media.
        m_delayedRestartOp = m_client->rtcRestartDelayedLeave(m_delayId);
        return;
    }
    // NO SERVER-SIDE CLEANUP EXISTS. The membership's own `expires` is the
    // only thing that will ever remove us, Rust has therefore published a
    // short one, and this is what keeps a LIVE participant from ageing out of
    // it. Re-publishing also re-arms a delayed retraction if the server has
    // meanwhile gained one.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // A clock that jumped BACKWARDS must not park the cadence: treat any
    // negative elapsed time as "due now".
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
    // Deliberately NOT guarded on an outstanding m_refreshOp. An answer that
    // never arrives must not disarm the heartbeat for the rest of the call;
    // the cadence above is what bounds the rate, and only the newest op id is
    // tracked because only the newest answer carries the live delay id.
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
    // ONE SIGNAL, TWO EVENTS. The bridge routes `rtc_membership_retracted`
    // and `rtc_delayed_updated` onto this one signal, and the op id is the
    // only thing that distinguishes them — so both are matched explicitly and
    // anything unrecognised is ignored rather than guessed at.
    if (m_delayedRestartOp != 0 && opId == m_delayedRestartOp) {
        m_delayedRestartOp = 0;
        if (ok)
            return;
        qCWarning(lcSfuCall)
            << "delayed leave restart FAILED category=" << category;
        // The delay id may have been CONSUMED — the server fired the
        // retraction because a restart arrived too late (an 8 s timeout
        // restarted every 5 s is a 3 s margin against an HTTP call whose own
        // timeout is 15 s). Our membership is then already gone from every
        // other client while we keep publishing media, and every later
        // restart of the same id 404s forever.
        //
        // Re-PUBLISHING is the repair, not another restart: it re-creates the
        // membership AND arms a fresh delayed retraction with a new id. The
        // id is cleared first so nothing keeps restarting a dead one.
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
    // Retry only what a retry can fix. A `forbidden` or a `not_found` will
    // not become true by asking again, and re-asking would only hide the
    // failure behind a longer silence.
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
    // The bridge refused to dispatch at all, so no answer will ever arrive
    // and the retry machinery would wait forever on it.
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
    // Logged unconditionally: a call that ends for a reason nobody can see is
    // the whole of "it just dies".
    qCInfo(lcSfuCall) << "teardown state=" << static_cast<int>(finalState)
                      << "error=" << (error.isEmpty()
                                      ? QStringLiteral("<none>") : error);
    ++m_generation;
    m_refreshTimer.stop();
    m_publishOp = 0;
    m_refreshOp = 0;
    m_delayedRestartOp = 0;
    m_lastPublishMs = 0;

#ifdef HAVE_LIGHTNING_WEBRTC
    // Media FIRST: release the microphone and camera before anything that
    // can fail or block. No device stays live because a network call hung.
    if (!m_engine.isNull())
        m_engine->stop();
#endif
    if (m_portal)
        m_portal->cancel();
    if (m_client) {
        m_client->sfuDisconnect();
        if (!m_roomId.isEmpty()) {
            // Retract our membership and cancel the delayed retraction. Safe
            // to issue even if we never got as far as publishing: the Rust
            // side treats an empty delay id as "nothing to cancel".
            //
            // This USED TO BE FIRE-AND-FORGET, with nothing listening to the
            // answer. A retraction is an ordinary network request issued at
            // the exact moment a user is often walking away from a flaky
            // connection, and a failed one left a membership in the room that
            // no later code ever removed. It is now a tracked, bounded,
            // retried attempt whose failure is at least reported.
            //
            // A retry in flight from a PREVIOUS leave is superseded: whatever
            // room this teardown is for, that is the one being left now.
            m_retractRetryTimer.stop();
            m_retractAttempts = 0;
            dispatchRetraction(m_roomId, m_delayId);
        }
    }

    m_roomId.clear();
    m_focusUrl.clear();
    m_membershipEventId.clear();
    m_delayId.clear();
    m_ownIdentity.clear();
    m_participants.clear();
    m_speaking.clear();
    m_speakingLevel.clear();
    m_connectionQuality.clear();
    // Emptied, never replaced: a bound view keeps the same model object and
    // sees a removeRows, not a rebind. The stage's view state goes with the
    // call it belonged to — a pin or a dismissed share from the last call
    // must not greet the user in the next one.
    if (m_participantModel)
        m_participantModel->clear();
    if (m_shareModel)
        m_shareModel->clear();
    if (m_stageState)
        m_stageState->clear();
    m_publishedTrackIds.clear();
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_cameraOn = false;
    m_screenSharing = false;
    m_handRaised = false;
    m_handReactionId.clear();
    m_handOp = 0;
    m_handReactions.clear();
    m_mediaEncrypted = false;
#ifdef HAVE_LIGHTNING_WEBRTC
    // The sink table belonged to THIS call's track sids. In practice the
    // tiles release their own as the models empty, but "in practice" is not
    // the bar for a table the engine consults on a STREAMING THREAD: a stale
    // entry is a dangling destination for the next call's frames, and sids
    // are server-assigned and can repeat.
    //
    // Unconditional, and it must stay that way. The ownership rule that
    // governs one surface's release is deliberately NOT applied here — there
    // is no surviving owner to protect, and honouring it would leave exactly
    // the stale entries this exists to remove.
    if (m_videoRouter)
        m_videoRouter->clear();
#endif
    // Mute/deafen intent deliberately SURVIVES a call (the familiar
    // convention); it is cleared only on sign-out, where setClient runs.
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
    // THE MIC GAIN IS PART OF THE AUDIO STATE, so it rides the one path that
    // every mute, deafen and session start already goes through rather than
    // needing its own call site nobody will remember to add. Applied at
    // session start (publishTracks -> applyAudioState) rather than on
    // `connected`, for the same reason mute is: `connected` arrives through a
    // queued marshal, at least one event-loop turn after RTP is flowing, so
    // applying it there publishes the opening window at the wrong level.
    m_engine->setMicrophoneGain(m_settings ? m_settings->microphoneGain()
                                           : 100);
#endif
    // OUTSIDE both guards, and that is deliberate. Telling the SFU our mute
    // state is pure SIGNALLING: it needs a client, not a GStreamer pipeline.
    // Inside the guard the entire mute lane was unreachable in
    // `call-controller-test`, which is not built with HAVE_LIGHTNING_WEBRTC,
    // so none of it could be exercised at any layer. Production behaviour is
    // unchanged — syncMicMuteToSfu() gates on `active()`, and a build or a
    // controller with no engine refuses to join at all, so this cannot send
    // anything a call did not ask for.
    //
    // Local muting stops packets; it does not tell anyone. Other clients read
    // the mute state off the TRACK, so without this the mic icon in Element
    // never moved — and a mute the SFU inferred from silence stayed set after
    // we started sending again, which is the reported "I unmute and remain
    // muted in Element".
    syncMicMuteToSfu();
}

void SfuCallController::setMicrophoneMuted(bool muted)
{
    if (m_micMuted == muted)
        return;
    m_micMuted = muted;
    // Unmuting by hand while deafened is contradictory, so it lifts the
    // deafen too rather than leaving the button saying live and the engine
    // saying silent.
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
        // Remember what to come back to: someone already muted must not be
        // published live again by undeafening.
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

void SfuCallController::setCameraOn(bool on)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_cameraOn == on || !active() || m_engine.isNull() || !m_client)
        return;
    m_cameraOn = on;
    if (on) {
        const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_client->sfuAddTrack(cid, QStringLiteral("camera"), 1,
                              SfuMediaEngine::kCameraWidth,
                              SfuMediaEngine::kCameraHeight,
                              false, m_roomEncrypted);
        m_engine->publishVideo(cid, false, -1);
        m_cameraCid = cid;
        m_publishedTrackIds.append(cid);
    } else {
        // Unpublish the CAMERA track by id. This used to take "the last
        // track we published", which is the SCREEN SHARE whenever the share
        // started after the camera — so turning the camera off killed the
        // share instead, and the camera stayed live. The camera must
        // actually go off: the LED is the user's only unambiguous indicator.
        unpublishTrack(m_cameraCid);
        // The self-view's sink is not told anything by a bin that simply
        // stopped existing, so it keeps painting its last frame. See
        // clearLocalVideoSurface().
        clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
    }
    // The SFU has to hear about it too, or every other client keeps seeing a
    // camera we turned off — and our own row reads back as still on.
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
    // A negative node id is REFUSED, never defaulted: the portal is what
    // decides which monitor or window is captured, and guessing here is
    // exactly how the wrong screen gets published.
    //
    // ...UNLESS A WINDOW WAS CHOSEN, which carries its own handle and no node
    // id at all. This guard has a twin in SfuMediaEngine::publishVideo and
    // only the twin was taught about windows, so picking a window returned
    // false RIGHT HERE — before a single line was logged, which is why the
    // report was "selecting a window and sharing does nothing" and the log
    // showed three `screen share requested` and not one publish.
    //
    // ...AND UNLESS AN X11 ROOT RECTANGLE WAS CHOSEN in the no-portal
    // fallback, which is the third source kind and carries no node id
    // either. Its TWIN lives in SfuMediaEngine::publishVideo and the two must
    // learn each new kind together — last time only the twin was taught about
    // windows, so a window share returned false right here, before a single
    // line was logged.
    if (pipewireNodeId < 0 && windowHandle == 0 && !captureRect.isValid())
        return false;
    if (m_screenSharing)
        stopScreenShare();
    const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // `window=` and `x11rect=` are BOOLEANS, never the values: an HWND
    // identifies a window on the user's desktop and its title is theirs, and
    // a root rectangle carries their monitor layout. Neither belongs in a log
    // they may be asked to send.
    qCInfo(lcSfuCall) << "screen share publishing node=" << pipewireNodeId
                      << "window=" << (windowHandle != 0)
                      << "x11rect=" << captureRect.isValid()
                      << "encrypted=" << m_roomEncrypted;
    m_client->sfuAddTrack(cid, QStringLiteral("screen"), 1,
                          SfuMediaEngine::kScreenWidth,
                          SfuMediaEngine::kScreenHeight,
                          true, m_roomEncrypted);
    m_engine->publishVideo(cid, /*screenShare=*/true, pipewireNodeId,
                           pipewireFd, windowHandle, captureRect);
    m_screenCid = cid;
    m_publishedTrackIds.append(cid);
    m_screenSharing = true;
    // A NEW share is a new identity for the stage. See m_localShareEpoch:
    // without this, stopping and restarting our own share would reuse one
    // share id, and a viewer who had dismissed the first from their
    // spotlight would silently never be offered the second.
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

void SfuCallController::stopScreenShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || m_engine.isNull())
        return;
    unpublishTrack(m_screenCid);
    m_screenSharing = false;
    // Close the portal session too: leaving it open keeps the compositor
    // capturing a surface nothing is reading.
    if (m_portal)
        m_portal->cancel();
    // The local share surface holds the last frame it was handed unless it is
    // told otherwise, and the tile that owns it may not be destroyed for
    // another frame or two. See clearLocalVideoSurface().
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
    // A QVideoSink KEEPS ITS LAST FRAME until it is given another one. Our
    // self-view is tee'd off the capture, so tearing the publishing bin down
    // ends the frames without ending the picture: the tile goes on showing a
    // still image of whatever was last shared, which reads as a share that
    // refused to stop. Reported exactly that way — "my video feed remains
    // frozen and doesnt seem to turn off and leaves a blank frame".
    //
    // A NULL QVideoFrame is how a sink is told "there is nothing here now":
    // `videoSize` goes invalid, so QML's `hasFrame` test goes false and the
    // placeholder comes back.
    //
    // Belt and braces with the row removal in rebuildModels(). That one
    // destroys the tile, which detaches its sink — but only once QML gets
    // round to it, and only if a tile is what owns the sink. This is
    // unconditional, costs a hash lookup, and cannot clear anyone else's
    // surface: the two local stream ids are ours by construction.
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
    // THE MICROPHONE GOES BOTH WAYS, and it is the only source that can.
    //
    // It is published exactly once per call and never republished, so our own
    // row carries at most one `microphone` track and the first match is
    // unambiguously it. Video cannot say that any more — see
    // muteOwnTrackIfLive().
    if (!m_client || !active())
        return;
    // The SERVER's track sid, never our client-chosen cid: MuteTrackRequest
    // names the published track as the SFU knows it.
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
            return; // declared but not yet named; the next ParticipantUpdate
                    // re-runs this, which is why it is called from
                    // onSfuParticipants at all
        // Only when the server's answer differs from ours. Reconciling
        // against the REPORTED state rather than remembering what we last
        // sent is what makes this converge: it starts from whatever the
        // server currently believes, including a mute it inferred itself, and
        // it cannot loop because the request changes the reported value.
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
    // MUTE ONLY. THIS DIRECTION IS THE ONLY SAFE ONE FOR VIDEO, and the
    // asymmetry is the whole subtlety of expressing a stop as a mute.
    //
    // Muting is safe because the target is self-identifying: a track the
    // server still reports as LIVE is one that ought not to be, and it
    // converges by report — once the server agrees there is no live track of
    // this source left and nothing more is sent.
    //
    // UNMUTING IS NOT SAFE AND IS NOT DONE. A stop leaves the muted corpse of
    // the old track listed (a mute removes nothing), so when the user starts a
    // new share the server may still be reporting ONLY the corpse — its sid is
    // server-assigned and nothing maps it back to the cid we published, so
    // "the screen_share track is muted and we want it live, unmute it" would
    // name the DEAD one and put a track producing no RTP back on the wire.
    // That is exactly the state this whole path exists to end.
    //
    // Nothing is lost by refusing. A video track is published FRESH every
    // time — setCameraOn and startScreenShare mint a new cid, and there is no
    // mute-in-place for video anywhere in this class — and the SFU reports a
    // freshly published track as unmuted, so an unmute is never needed here.
    // The microphone, which genuinely does mute in place, has its own path
    // above.
    for (const QVariant &t : own.value(QStringLiteral("tracks")).toList()) {
        const QVariantMap track = t.toMap();
        if (track.value(QStringLiteral("source")).toString() != source)
            continue;
        if (track.value(QStringLiteral("muted")).toBool())
            continue;
        const QString sid = track.value(QStringLiteral("sid")).toString();
        if (sid.isEmpty())
            continue; // declared but not yet named; the next
                      // ParticipantUpdate re-runs this
        // No `return`: if the server somehow lists two live tracks of one
        // source, the user stopped BOTH and both must be silenced.
        m_client->sfuMuteTrack(sid, true);
    }
}

void SfuCallController::applyVideoState()
{
    // TELL THE SFU THE VIDEO TRACK STOPPED. Nothing else does.
    //
    // There is no unpublish verb on this wire at all: the client seam offers
    // exactly `sfuAddTrack` and `sfuMuteTrack`, and rust/src/sfu.rs sends
    // only Ping/PingReq/Offer/Answer/Trickle/AddTrack/Mute/Leave. Stopping a
    // share therefore tore down the LOCAL GStreamer bin and told the server
    // nothing, so the SFU went on listing an unmuted `screen_share` track for
    // us — which is what every other client in the call was still being
    // offered, and (the hypothesis for the red warning triangle on the
    // maintainer's own tile) what a SFU scores as a poor publisher: a live,
    // unmuted video track producing zero RTP.
    //
    // A mute is the one removal-shaped verb available on the wire, and it is
    // the same one the microphone has always used.
    //
    // IT IS NO LONGER THE ONLY MECHANISM, and it is no longer the load-
    // bearing one. `SfuMediaEngine::unpublish()` now releases its webrtcbin
    // request pad, which retires the transceiver, so the renegotiated offer
    // genuinely stops advertising the track — pinned by
    // theOfferAfterUnpublishNoLongerAdvertisesTheTrack. That is what actually
    // makes the far end drop the tile, because a MUTE REMOVES NOTHING: the
    // stopped track stays in the participant list forever, which is why a
    // stopped share went on being rendered as a corpse and a new one landed
    // beside it ("its like the first one doesnt stop").
    //
    // The mute is KEPT as belt-and-braces: it costs one signal, it converges
    // by report, and it covers the window between the user stopping and the
    // renegotiation landing. It must not be relied on alone again.
    // Only ever a MUTE, and only for a source the user has turned OFF: see
    // muteOwnTrackIfLive() for why the other direction is refused.
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

    // ON THE WIRE, in element-call's own format — read out of its source
    // (`src/reactions/useReactionsSender.tsx`), not invented here:
    //
    //   raise  → an `m.reaction` annotating OUR OWN `m.call.member` state
    //            event with U+1F590 U+FE0F
    //   lower  → a redaction of that reaction
    //
    // Annotating the MEMBERSHIP rather than a timeline message is what scopes
    // the hand to one call: a new membership (rejoining, refreshing) is a new
    // event, so an old hand cannot follow the user into the next call.
    if (m_client && !m_roomId.isEmpty()) {
        // Our own membership event id. Preferring the RtcController's
        // observation over m_membershipEventId matters after a REFRESH: the
        // periodic re-publish replaces the state event, and annotating the
        // one we published at join would address an event the room has
        // superseded — a hand nobody would ever see.
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
    // OPTIMISTIC ON OUR OWN ROW ONLY, and deliberately: this is a toggle the
    // user is watching, the round trip is a second or more, and a control
    // that does nothing for a second reads as broken. A REFUSAL puts it back
    // (onHandResult), which is the same discipline the mute controls use.
    //
    // Nothing else is optimistic: a remote hand is only ever drawn from an
    // event that actually arrived.
    if (m_participantModel && !m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    // Lowering forgets the reaction immediately. The redaction is in flight
    // and the id is spent either way; keeping it would let a later lower
    // redact an event that is already gone.
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
        // The id the eventual lower must redact. Without keeping it a raised
        // hand can never be lowered by this device.
        if (raised)
            m_handReactionId = eventId;
        return;
    }
    // Put the control back. `category` is a sanitized class, never a body.
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
        // A REDACTION NAMES ONLY WHAT IT REMOVED. The reaction is gone, so
        // nothing on the wire can say whose hand it was — we answer from the
        // ids we are already holding. A redaction of anything else is not
        // ours and is dropped here, which is why every redaction in every
        // room can be forwarded cheaply.
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

    // A RAISE is attributed through the membership it annotates, and
    // identityForMembership refuses a sender who does not own that
    // membership — anyone may annotate anyone's state event.
    if (!m_rtc)
        return;
    const QString identity =
        m_rtc->identityForMembership(roomId, membershipEventId, sender);
    if (identity.isEmpty())
        return;
    m_handReactions.insert(reactionEventId, identity);
    m_participantModel->setHandRaised(identity, true);
    if (identity == m_ownIdentity) {
        m_handReactionId = reactionEventId;
        if (!m_handRaised) {
            m_handRaised = true;
            Q_EMIT mediaStateChanged();
        }
    }
}

void SfuCallController::onHandsReceived(quint64 opId, const QString &roomId,
                                        const QVariantList &hands)
{
    Q_UNUSED(opId);
    if (roomId != m_roomId || m_roomId.isEmpty() || !m_participantModel)
        return;
    // The join-time sweep. It ADDS what it found and never clears: a hand
    // this pass could not read is not a hand that is down, and lowering one
    // on the strength of a failed read is the one wrong answer available.
    for (const QVariant &value : hands) {
        const QVariantMap hand = value.toMap();
        const QString identity =
            hand.value(QStringLiteral("rtcIdentity")).toString();
        const QString reactionId =
            hand.value(QStringLiteral("reactionEventId")).toString();
        if (identity.isEmpty() || reactionId.isEmpty())
            continue;
        m_handReactions.insert(reactionId, identity);
        m_participantModel->setHandRaised(identity, true);
        if (identity == m_ownIdentity) {
            // Our own hand, still up from before this join. Adopt it rather
            // than leaving a raised hand nobody here can lower.
            m_handReactionId = reactionId;
            if (!m_handRaised) {
                m_handRaised = true;
                Q_EMIT mediaStateChanged();
            }
        }
    }
}

void SfuCallController::toggleHandRaised() { setHandRaised(!m_handRaised); }

QString SfuCallController::userIdForIdentity(const QString &identity) const
{
    if (identity.isEmpty() || !m_rtc)
        return {};
    // THE MEMBERSHIP RESOLVES THIS, never string surgery on the identity.
    // `@user:server:DEVICE` splits on ':' by luck in the legacy format and
    // not at all in the sticky one, whose identity is a base64 sha256 — and
    // §16 records that trap costing a round already. Empty is UNKNOWN.
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

void SfuCallController::setParticipantVolume(const QString &identity,
                                              int percent)
{
    const int clamped = qBound(0, percent, 200);
#ifdef HAVE_LIGHTNING_WEBRTC
    // Local only: nothing is sent, and nobody else is affected. The ENGINE is
    // addressed by LiveKit stream id, not by SFU identity — that is the name
    // the receive bin's volume element carries, and the two are different
    // strings.
    const QString streamId = streamIdForIdentity(identity);
    if (!m_engine.isNull() && !streamId.isEmpty()) {
        m_engine->setParticipantVolume(streamId, clamped);
    } else {
        // Reported twice as a control that "does nothing, but does remember
        // the set %": the value lands in settings below whatever happens
        // here, so a silent miss at this line looks exactly like success.
        // Booleans and a sid, no user content.
        qCWarning(lcSfuCall)
            << "participant volume not applied: engine="
            << !m_engine.isNull() << "streamId=" << streamId
            << "participants=" << m_participants.size();
    }
#endif
    // Recorded so the control that sets it can READ IT BACK. This was
    // write-only, which is why no QML ever called it: a slider with nothing
    // to bind to cannot show the value it just set.
    if (m_participantModel)
        m_participantModel->setVolumePercent(identity, clamped);
    // PERSISTED under the person, not under the session. The user id comes
    // from the membership; if it is not known yet the value still applies to
    // this call and is simply not remembered, which is better than storing it
    // under a device-scoped key that will never be looked up again.
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
        // NEVER the local row. This is a PLAYBACK volume on a receive chain,
        // and we do not receive ourselves — there is nothing to turn down,
        // and offering it would be a control that does nothing.
        if (person.value(QStringLiteral("local")).toBool())
            continue;
        const QString identity =
            person.value(QStringLiteral("identity")).toString();
        const QString userId = userIdForIdentity(identity);
        if (identity.isEmpty() || userId.isEmpty())
            continue; // unknown person: unity, and nothing invented
        const int stored = m_settings->callParticipantVolume(userId);
        if (person.value(QStringLiteral("volumePercent")).toInt() == stored)
            continue;
        m_participantModel->setVolumePercent(identity, stored);
#ifdef HAVE_LIGHTNING_WEBRTC
        const QString streamId = streamIdForIdentity(identity);
        if (!m_engine.isNull() && !streamId.isEmpty())
            m_engine->setParticipantVolume(streamId, stored);
#endif
    }
}

// ONE derivation of the participant rows, feeding the model; the model then
// feeds everything else, including participants().
//
// It used to be the other way round: participants() rebuilt a QVariantList
// from scratch every time QML asked, and QML asked whenever a hand-bumped
// tick changed. A JS array reassigned into a view is a MODEL RESET, so a
// speaker update destroyed every tile and every VideoOutput in it. Building
// rows here and DIFFING them into the model turns the same information into
// insert/remove/move plus per-role dataChanged.
void SfuCallController::rebuildModels()
{
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // A STAGED CALL OWNS THE MODEL. This function reconciles against the SFU,
    // and a demo call has none — so every rebuild would clear the fictional
    // participants back to nothing. It is reached from `mediaStateChanged`
    // (which the staging itself emits) and from the refresh timer, so a
    // guard at the one place they both arrive is the only one that holds.
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
        // Resolved through the MatrixRTC MEMBERSHIP, never by string
        // surgery on the identity.
        //
        // This used to split the identity on its last colon to recover a
        // user id, which works for the legacy `@user:server:DEVICE` form and
        // produces garbage for the sticky format — whose identity is an
        // unpadded base64 sha256. A remote participant therefore rendered as
        // a chunk of random symbols with no display name and no avatar.
        const QVariantMap person = m_rtc
            ? m_rtc->participantForIdentity(m_roomId, identity)
            : QVariantMap{};
        CallParticipantRow row;
        row.identity = identity;
        row.sid = entry.value(QStringLiteral("sid")).toString();
        row.userId = person.value(QStringLiteral("userId")).toString();
        // Room-resolved profile, so a tile draws a real name and avatar.
        // Empty means "not known here" and the tile falls back to initials
        // rather than inventing anything.
        row.displayName =
            person.value(QStringLiteral("displayName")).toString();
        row.avatarMxc = person.value(QStringLiteral("avatarMxc")).toString();
        // "local" is this DEVICE. The membership knows; identity equality is
        // kept as the fallback for a session whose membership has not landed
        // yet, so the local tile is never mislabelled as someone else.
        const bool ownDevice =
            person.value(QStringLiteral("ownDevice")).toBool();
        row.local = ownDevice
            || (!m_ownIdentity.isEmpty() && identity == m_ownIdentity);

        // Track state as the SFU reports it. Absent means UNKNOWN, and the
        // UI must render nothing rather than a confident "not muted".
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
                // OR, not assignment: a participant can now legitimately
                // carry two tracks of one source, because a stop is a MUTE on
                // this wire and the muted one is not removed. Letting the
                // last entry decide would report a live camera as off
                // whenever the dead track happened to be listed second — and
                // it would then disagree with trackKeyForSource(), which
                // prefers the live sid.
                row.cameraOn = row.cameraOn || !muted;
            } else if (source == QLatin1String("screen_share")) {
                row.screenSharing = row.screenSharing || !muted;
            }
        }
        // The routing keys, so a tile can RE-ATTACH when they change. The SFU
        // can announce a participant before it announces which track their
        // media landed on, and an attach that happened while the key was
        // still empty is a surface that never receives a frame. QML watches
        // these two values; it does not interpret them.
        row.cameraTrackKey =
            trackKeyForSource(identity, QStringLiteral("camera"));
        row.screenTrackKey =
            trackKeyForSource(identity, QStringLiteral("screen_share"));

        if (row.local) {
            sawLocal = true;
            // OUR OWN state is authoritative HERE, not at the SFU.
            //
            // The server learns our camera and share only once the track is
            // published and announced, so between the user pressing the
            // button and that round trip the local tile said "camera off"
            // while the capture light was on — and the local screen-share
            // surface, which gates on this flag, drew nothing at all. We know
            // what we asked for, in BOTH directions. applyAudioState() and
            // applyVideoState() converge the server towards our intent, so
            // reading any of it back from the server was reading our own
            // intent through a delay — and, on the way down, never getting
            // there at all.
            row.micKnown = true;
            row.micMuted = m_micMuted;
            row.cameraKnown = true;
            // ASSIGNMENT, NOT `||`. This used to OR our intent into whatever
            // the SFU last reported, which only ever repaired the LEADING
            // edge — and turning something OFF is the other edge.
            //
            // Nothing tells the SFU a track ENDED (there is no unpublish verb
            // on the wire; applyVideoState() now mutes instead), so after a
            // stop the server keeps reporting `{source: screen_share,
            // muted: false}` for us. The OR then discarded our authoritative
            // `false`, the local share row survived, `CallShareTile` was
            // never destroyed, its `Component.onDestruction: detach()` never
            // ran, and the self-view kept painting its last frame — reported
            // as "when i stop screen share my video feed remains frozen and
            // doesnt seem to turn off". The mic already had this rule (one
            // line up) and was already right.
            row.cameraOn = m_cameraOn;
            row.screenSharing = m_screenSharing;
        }
        rows.append(row);
    }

    // The SFU's join payload lists the OTHERS; our own row arrives with the
    // first update about us, which can be a moment later. A call surface
    // that cannot show the local user until the server mentions them is the
    // "1 person in call" shape from the other direction, so a placeholder
    // stands in — keyed on the same identity, so the real row REPLACES it
    // rather than duplicating it.
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
    // A row that has only just appeared has never seen a speakers round, and
    // the next one may be seconds away. Re-apply what we last heard so a
    // tile is not born silent for someone who is mid-sentence.
    m_participantModel->applySpeakers(m_speaking, m_speakingLevel);
    m_participantModel->applyConnectionQuality(m_connectionQuality);
    // A row that has only just appeared is born at unity. Somebody the user
    // turned down in a previous call — possibly in a different room — must
    // come back at the volume they chose, which is the whole of the request.
    applyStoredVolumes();
    // Hand raise has no wire representation (see CallParticipantModel), so
    // the only row it can be true for is ours.
    if (!m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    rebuildShareModel();
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
            // See m_localShareEpoch: our own share exists before the SFU has
            // named a track for it, so it cannot be keyed on the sid — and
            // reusing one id across a stop/start would let a dismissal from
            // the first share suppress the second.
            share.shareId = QStringLiteral("local:%1").arg(m_localShareEpoch);
        } else {
            // The TRACK sid. A share that stops and restarts is a new
            // published track and therefore a new id, which is exactly what
            // keeps a stale dismissal from suppressing it.
            share.shareId = share.trackKey;
        }
        if (share.shareId.isEmpty())
            continue; // a remote share with no track stated yet is not
                      // addressable, and a row nothing can attach to is
                      // worse than no row
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
    m_lastPublishMs = 0;
}

void SfuCallController::setCallStateForTest(State state)
{
    // Only the STATE, so the `active()` gates that guard every wire-touching
    // path behave as they do in a real call. Nothing else about a join is
    // simulated, and nothing here is reachable from QML.
    m_state = state;
}

void SfuCallController::setLocalMediaStateForTest(bool cameraOn,
                                                  bool screenSharing)
{
    // The half of setCameraOn()/startScreenShare()/stopScreenShare() that
    // does not need a media engine: record the intent, tell the SFU, rebuild.
    // It goes through the SAME applyVideoState() production uses, because the
    // thing worth testing is that a local stop reaches both the model and the
    // wire — and §16 records twice what a test that invokes a policy
    // function production never reaches is worth.
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
