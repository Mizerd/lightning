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
#include "calls/noise/MicProcessing.h"
#include "app/SandboxEnvironment.h"
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
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
// The staged demo call's shared-screen still (attachScreenSink).
#include <QImage>
#include <QVideoFrame>
#include <QVideoSink>
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
    // Never QFileInfo::exists("/.flatpak-info"): see SandboxEnvironment.h.
    return sandboxenv::isSandboxed();
#else
    return false;
#endif
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
/// The `expires` window rust/src/rtc.rs writes when no MSC4140 delayed
/// retraction is armed (MEMBERSHIP_EXPIRY_NO_DELAYED_MS; change the two
/// together). Readers date it from the last accepted write, so that write
/// plus this is when the membership stops counting for everyone.
constexpr qint64 kMembershipExpiryNoDelayedMs = 5 * 60 * 1000;
static_assert(kMembershipRepublishIntervalMs < kMembershipExpiryNoDelayedMs,
              "the re-publish cadence must beat the membership's expiry");
/// How long a retraction that failed for transient (network, rate limit)
/// reasons stays owed, counted from the last write the server accepted: the
/// membership's own expiry plus slack for clock skew and a slow last
/// attempt. Past it every reader has dropped the membership, so nothing is
/// sent. This, not an attempt count, is the bound: a network outage outlasts
/// any short retry chain, and a dropped retraction is a ghost participant.
constexpr qint64 kRetractOwedForMs = kMembershipExpiryNoDelayedMs + 60 * 1000;
/// The `expires` rust/src/rtc.rs writes first, assuming MSC4140 will clean up
/// (MEMBERSHIP_EXPIRY_MS; change the two together). When that write landed
/// but neither the delayed retraction nor the short fallback write did, this
/// is the membership's real lifetime, and the retraction stays owed for it.
constexpr qint64 kMembershipExpiryLongMs = 4LL * 60 * 60 * 1000;
constexpr qint64 kRetractOwedLongForMs = kMembershipExpiryLongMs + 60 * 1000;
/// An attempt whose answer has not arrived after this counts as failed. Rust
/// bounds the write and the delayed-event cancel at 15 s each, so an answer
/// later than this was lost (an event-queue overflow), not slow.
constexpr int kRetractAnswerTimeoutMs = 35 * 1000;
/// Backoff for an owed retraction: doubling from the first delay up to the
/// ceiling, so a long outage costs about one attempt a minute. The homeserver
/// becoming reachable again sends it at once (onHomeserverReachable).
constexpr int kRetractRetryDelayMs = 2000;
constexpr int kRetractRetryMaxDelayMs = 60 * 1000;
/// How long a rotated key is sent ahead of our frames switching to it, so
/// peers hold it before the first frame they must decrypt with it. Measured:
/// our to-device keys reached a Lightning peer 0.1-1.6 s after sending
/// typically, and up to 4.7 s (sliding sync). Counted from the send's answer,
/// as matrix-js-sdk waits `useKeyDelay` after `await sendKey`
/// (RTCEncryptionManager.rolloutOutboundKey; 1 s by default there, too short
/// for us): our send runs a /keys/query per due user and claims one-time keys
/// before its PUT, which with federated peers can take seconds.
constexpr int kUseKeyDelayMs = 5000;
/// The switch happens at the latest this long after a rotated key was
/// dispatched, answered or not, so a lost answer cannot keep the old key in
/// use for ever, and a leaver's window has a bound. Longer than the delay, or
/// it would cut the delay short; only a send slower than the difference gets
/// less than the full delay.
constexpr int kKeySwitchFallbackMs = 15000;
/// A joiner is sent our newest key when it is younger than this, and gets a
/// fresh key (a rotation) otherwise. matrix-js-sdk's
/// `keyRotationGracePeriodMs`, the same value.
constexpr int kJoinKeyGraceMs = 10000;
/// Our key indices wrap here. LiveKit rings hold 16 by default
/// (livekit-client KEY_PROVIDER_DEFAULTS) and index frames by the raw byte;
/// the bridge refuses anything above 15.
constexpr int kSendKeyIndices = 16;
/// Presentation bound on the participant list.
constexpr int kMaxParticipants = 64;
/// Annotations (raises, reactions) waiting for the membership they address.
/// One per participant is the real bound.
constexpr int kMaxPendingAnnotations = kMaxParticipants;

// ── Reconnecting after a transient network loss ──
//
// The SFU telling us to come back (Leave RESUME/RECONNECT, which is how it
// reports a media timeout), the signalling socket dying, or our ICE failing
// does not end the call: the membership, the call UI and the keys stay, and
// the SFU session is joined again from scratch (new JWT, websocket and peer
// connections; webrtcbin cannot restart ICE in place, so LiveKit's lighter
// "resume" is out of reach). A reconnect counts as done only when a new peer
// connection's ICE is up, not at the join: with the network still down the
// websocket can come back while media cannot.
//
// The first attempt goes after kReconnectFirstBackoffMs, then doubling to
// kReconnectMaxBackoffMs, each attempt bounded by kReconnectAttemptTimeoutMs
// (livekit-client's peerConnectionTimeout), the whole episode by
// kReconnectBudgetMs, after which the call ends with the existing "connection
// was lost" wording. The SFU itself gives up on a silent participant after
// about 15 s, so a network gone for 15-30 s recovers and one gone for more
// than about a minute ends honestly.
constexpr int kReconnectFirstBackoffMs = 1000;
constexpr int kReconnectMaxBackoffMs = 8000;
constexpr int kReconnectAttemptTimeoutMs = 15000;
constexpr int kReconnectBudgetMs = 45000;
/// An ICE transport that reports "disconnected" may come back by itself
/// (libnice keeps checking); only one that stays so this long reconnects.
constexpr int kIceDisconnectedGraceMs = 10000;
/// At most this many reconnect episodes may BEGIN within
/// kReconnectEpisodeWindowMs; the next loss ends the call. Each episode is
/// bounded by kReconnectBudgetMs, but a successful one resets that budget,
/// and the focus is chosen by the oldest membership (attacker input, see
/// docs/security-audit-2026-09-02.md): an SFU that lets ICE connect and then
/// sends Leave RESUME, or one that flaps, would otherwise cycle us for ever,
/// each cycle a fresh OpenID token POSTed to its JWT service and possibly a
/// key rotation fanned out over to-device.
constexpr int kMaxReconnectEpisodes = 3;
constexpr qint64 kReconnectEpisodeWindowMs = 10 * 60 * 1000;
} // namespace

SfuCallController::SfuCallController(QObject *parent) : QObject(parent)
{
    // The notice on screen is whatever callFailed() said last. A withdrawal
    // (an empty callFailed) clears it for everyone, so each withdrawer first
    // checks that the notice showing is still its own; see withdrawNotice().
    connect(this, &SfuCallController::callFailed, this,
            [this](const QString &message) { m_shownNotice = message; });
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
    // The share-audio application list, polled only while watched or sharing
    // (updateShareAudioAppsPolling()).
    m_shareAudioAppsTimer.setInterval(1500);
    connect(&m_shareAudioAppsTimer, &QTimer::timeout, this,
            &SfuCallController::refreshShareAudioApplications);
    // Also reconcile the key lane on each tick; otherwise a failed
    // distribution's retry is only reached by a changed membership read.
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
        // Without this an attempt whose answer was lost would hold the slot
        // for ever: every later trigger waits on an op in flight.
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
        // answered=false: no answer from a send to the holders of the key in
        // use; true: one came so late the delay is cut short.
        qCWarning(lcSfuCall)
            << "media key switch at the fallback,"
            << m_keySwitchFallbackTimer.interval()
            << "ms after the send index=" << m_newestKey.index
            << "answered=" << m_useKeyTimer.isActive();
        adoptNewestKey();
    });
    m_joinKeyGraceMs = kJoinKeyGraceMs;
    m_keyClock.start();

    // Reconnecting; see kReconnectBudgetMs.
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
    abandonOutstandingMembershipWrites();
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::rtcMembershipPublished, this,
            &SfuCallController::onMembershipPublished);
    // Failed retractions (offline, 5xx, rate limit) are logged and retried;
    // a leftover membership poisons the call for every other client.
    connect(m_client, &MatrixClient::rtcMembershipRetracted, this,
            &SfuCallController::onMembershipRetracted);
    // An owed retraction is sent the moment the homeserver answers again,
    // not at the next backoff step.
    connect(m_client, &MatrixClient::homeserverReachable, this,
            &SfuCallController::onHomeserverReachable);
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
                const bool camera = !cid.isEmpty() && cid == m_cameraCid;
                qCInfo(lcSfuCall)
                    << "sfu published our track kind="
                    << (cid == m_audioCid ? "microphone"
                                          : camera ? "camera" : "other")
                    << "sid=" << sid;
                // A camera accepted after the camera's notice: the notice no
                // longer applies. Never the failed track's own late answer.
                // The SFU accepting a track is not a first frame; a camera
                // that fails after it shows its own notice.
                if (camera && cid != m_cameraNoticeCid
                    && !m_cameraNotice.isEmpty()) {
                    if (withdrawNotice(m_cameraNotice)) {
                        qCInfo(lcSfuCall)
                            << "a camera works again; its notice withdrawn";
                    }
                    m_cameraNotice.clear();
                    m_cameraNoticeCid.clear();
                }
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
            [this](quint64 op, bool ok, const QString &category, int delivered,
                   int keyIndex) {
                // Only this call's sends may change its key state: an answer
                // to the previous call's send can land after a rejoin.
                const auto it = m_keySendOps.constFind(op);
                const bool thisCall = it != m_keySendOps.cend();
                const KeySend send = thisCall ? it.value() : KeySend{};
                if (thisCall)
                    m_keySendOps.remove(op);
                if (ok) {
                    qCInfo(lcSfuCall) << "media key sent index=" << keyIndex
                                      << "delivered=" << delivered
                                      << "thisCall=" << thisCall;
                    // Somebody holds this key; see adoptNewestKey().
                    if (thisCall && delivered > 0)
                        m_deliveredKeyIndex = keyIndex;
                    // The switch counts from here, not from the dispatch.
                    if (thisCall && delivered > 0)
                        startUseKeyDelay(send.serial, send.reachesKeyInUse);
                    return;
                }
                qCWarning(lcSfuCall)
                    << "media key NOT sent index=" << keyIndex
                    << "category=" << category << "delivered=" << delivered
                    << "thisCall=" << thisCall;
                // Those devices do not hold the newest key after all, so the
                // next reconciliation sends it to them again. They stay
                // recipients: if one leaves, we still rotate.
                if (thisCall && send.serial == m_newestKey.serial) {
                    for (const QString &device : send.devices)
                        m_keyHolders.remove(device);
                }
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
    // ICE: a failed transport reconnects the call, and a new session's
    // transport coming up is what ends a reconnect.
    connect(m_engine, &SfuMediaEngine::transportStateChanged, this,
            &SfuCallController::onEngineTransportState);
    // Not failed(): onEngineFailed ends the call, and one broken capture
    // device must not.
    connect(m_engine, &SfuMediaEngine::publishFailed, this,
            &SfuCallController::onEnginePublishFailed);
    // The microphone came back on the engine's slow retry: its notice is no
    // longer true (only withdrawn if it is still the one showing).
    connect(m_engine, &SfuMediaEngine::microphoneRecovered, this,
            [this](const QString &) {
                withdrawNotice(
                    userFacingError(QStringLiteral("audio_source_failed")));
            });
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
    connect(m_engine, &SfuMediaEngine::remotePlaybackFailed, this,
            &SfuCallController::onRemotePlaybackFailed);
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
    // The in-call meter, from the same `level` element. Monotonic: the
    // throttle measures intervals.
    connect(m_engine, &SfuMediaEngine::localAudioLevel, this,
            [this](double peakDb) {
                static QElapsedTimer monotonic;
                if (!monotonic.isValid())
                    monotonic.start();
                noteMicrophoneLevelAt(peakDb, monotonic.elapsed());
            });
    // The selected noise suppressor started, or failed and WebRTC's (or, with
    // no webrtcdsp, nothing) runs instead: say so where the choice is made.
    connect(m_engine, &SfuMediaEngine::noiseSuppressionStatus, this,
            [this](const QString &mode, bool ok) {
                setNoiseSuppressionFailedMode(
                    ok ? QString() : mode,
                    !ok && m_engine
                        && m_engine->noiseSuppressionFellBackToWebrtc());
            });
    // What the share's sound is carrying, so "the far end hears nothing" is
    // visible here instead of only in a log.
    connect(m_engine, &SfuMediaEngine::shareAudioLevel, this,
            [this](double peakDb) {
                // Below -60 dBFS is silence for this purpose: a paused video
                // or an idle application, not something anyone would hear.
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
    // What the share's sound track actually is (it can have fallen back to
    // the output monitor), so the status line never claims more than that.
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
    // Noise suppression switches live, through the same path.
    connect(m_settings, &SettingsManager::noiseSuppressionModeChanged, this,
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
                const bool reconnecting = m_state == State::Reconnecting;
                if (!active() || reconnecting
                    || !startScreenShare(static_cast<int>(nodeId),
                                         pipewireFd)) {
                    qCWarning(lcSfuCall)
                        << "screen share refused after portal grant active="
                        << active() << "reconnecting=" << reconnecting;
                    closePortalFd(pipewireFd);
                    // The granted session would keep the compositor
                    // capturing for nobody.
                    if (m_portal)
                        m_portal->cancel();
                    if (reconnecting)
                        Q_EMIT callFailed(shareWhileReconnectingMessage());
                }
            });
    connect(m_portal, &ScreenCastPortal::cancelled, this, [] {
        // The user declined; no message needed.
    });
    connect(m_portal, &ScreenCastPortal::failed, this,
            [this](const QString &category) {
                qCWarning(lcSfuCall) << "screen share portal failed category="
                                     << category;
#if defined(HAVE_LIGHTNING_WEBRTC) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
                // KDE's portal refuses on X11 ("not available in X11
                // sessions"), refuses the session, or never answers at all
                // (bounded in requestScreenShare()); Lightning's own picker
                // works there.
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
                    offerLinuxDisplayPicker(/*autoSelectSingle=*/false);
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
                // A portal that could not hand out a remote is not the user
                // saying no: with device access granted, open the camera
                // directly instead (cameraFailureStep()).
                if (m_cameraAwaitingPortal && active()
                    && cameraFailureStep(category, /*viaPortal=*/true,
                                         cameraDeviceNodeVisible(),
                                         m_cameraFellBack)
                        == CameraFailureStep::OpenDeviceNode) {
                    m_cameraAwaitingPortal = false;
                    fallBackToDeviceCamera(category);
                    return;
                }
                abandonPendingCamera();
                // No track was declared.
                announceCameraNotice(
                    cameraNoticeCategory(category, /*viaPortal=*/true,
                                         m_cameraPortalReportedCamera),
                    QString());
            });
}

SfuCallController::LinuxCameraRoute SfuCallController::linuxCameraRoute(
    bool sandboxed, bool portalUsable, bool directDeviceVisible)
{
    // 1. A sandbox with a visible device node AND no usable portal camera:
    //    the user granted device access themselves (Flatseal's "Webcam",
    //    `flatpak override --device=all`, a connected snap camera interface)
    //    and the host has no PipeWire camera for the portal to hand out
    //    (Debian 12 by default), so the portal would answer with a remote
    //    holding no camera node: the camera "registers" with no picture
    //    (reported 2026-10-06). A usable portal still wins: MIPI/IPU6/libcamera
    //    laptops list /dev/video* nodes that raw v4l2src cannot use, and the
    //    portal camera already works there.
    if (sandboxed && directDeviceVisible && !portalUsable)
        return LinuxCameraRoute::Direct;
    // 2. A sandbox with no device node: the portal is the only possible
    //    camera, even when probes say it is unusable (its refusal is at least
    //    actionable).
    if (sandboxed)
        return LinuxCameraRoute::Portal;
    // 3. A visible device node: the direct path, as on any normal desktop.
    if (directDeviceVisible)
        return LinuxCameraRoute::Direct;
    // 4. Nothing to open directly, but the portal has a camera.
    if (portalUsable)
        return LinuxCameraRoute::Portal;
    // 5. Direct, including its honest failure.
    return LinuxCameraRoute::Direct;
}

SfuCallController::CameraFailureStep SfuCallController::cameraFailureStep(
    const QString &category, bool viaPortal, bool directDeviceVisible,
    bool alreadyFellBack)
{
    if (!viaPortal || !directDeviceVisible || alreadyFellBack)
        return CameraFailureStep::Notice;
    // The portal route produced nothing, or could not start because there is
    // no portal or no PipeWire behind it. Deliberately not `not_allowed` (the
    // desktop refused the camera: a lockdown, or a stored "no"), not
    // `portal_failed` (an error that is neither absence nor refusal), not
    // `timeout` (an unanswered permission dialog) or `busy`; and a decline
    // never arrives here at all.
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
    // The desktop refused the camera (a lockdown): say so; nothing else
    // would make sense of a camera that will not turn on.
    if (category == QLatin1String("not_allowed"))
        return QStringLiteral("camera_not_allowed");
    // The rest of CameraPortal's failure categories: the same fact as a
    // device that will not open.
    if (category == QLatin1String("no_pipewire_remote")
        || category == QLatin1String("portal_failed")
        || category == QLatin1String("timeout")
        || category == QLatin1String("busy"))
        return QStringLiteral("camera_failed");
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
    // Flatseal labels `--device=all` "All devices (e.g. webcam)"; the same
    // grant is `flatpak override --user --device=all <app id>`.
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

void SfuCallController::fallBackToDeviceCamera(const QString &category)
{
    m_cameraFellBack = true;
    qCWarning(lcSfuCall) << "camera portal gave no picture; opening the "
                            "device node directly category="
                         << category;
    // Whatever the portal route declared is gone (unpublished by the caller,
    // or never declared): tell the SFU before the replacement is declared,
    // exactly as turning the camera off and on again would.
    m_cameraOn = false;
    applyVideoState();
    m_cameraOn = true;
    m_cameraViaPortal = false;
    publishCameraTrack(/*pipewireFd=*/-1);
    Q_EMIT mediaStateChanged();
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

bool SfuCallController::portalFailureFallsBackToDisplays(
    const QString &category, const QString &platformName,
    const QString &sessionType, const QString &waylandDisplay,
    const QString &x11Display, bool captureElementPresent)
{
    // The portal's own X11 refusal; a portal that refused to make a session
    // (no dialog was shown, so the user declined nothing); and one that never
    // answered (the caller's no-answer bound, or the overall timeout). The
    // picker that follows always asks, so none of these captures anything
    // without the user's choice. A dismissal in the portal's picker arrives
    // as `cancelled`, never here.
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

void SfuCallController::offerLinuxDisplayPicker(bool autoSelectSingle)
{
#if defined(HAVE_LIGHTNING_WEBRTC) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // Also reached from the portal's failure handler, which can land after a
    // reconnect began.
    if (m_state == State::Reconnecting) {
        Q_EMIT callFailed(shareWhileReconnectingMessage());
        return;
    }
    if (!populateLinuxDisplaySources()) {
        Q_EMIT callFailed(tr("No display is available to share."));
        return;
    }
    Q_EMIT screenShareSourcesChanged();
    // A single display is not a choice, except after a portal refusal: the
    // user never consented to capture, so they always pick.
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
    // Nothing can be published while reconnecting; asking the user to pick
    // (and consent in the portal) first would only end in a refusal.
    if (m_state == State::Reconnecting) {
        Q_EMIT callFailed(shareWhileReconnectingMessage());
        return;
    }
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
    // Empty off Windows. A window owned by a process Lightning runs inside
    // (explorer.exe, a launcher, a terminal) must not lend its sound: that
    // process's tree includes our own playback, the echo.
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
            // The application, separately from the caption (a browser
            // window's caption is just the tab title).
            { QStringLiteral("application"), window.application },
            { QStringLiteral("geometry"),
              QStringLiteral("%1 x %2").arg(window.width).arg(window.height) },
            { QStringLiteral("primary"), false },
            { QStringLiteral("current"), false },
            // The share-audio key of the window's application (its
            // executable, as ShareAudioSources keys Windows sessions), so the
            // picker can offer "this window's sound". Empty when unreadable.
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
    case LinuxShareRoute::Portal: {
        // Monitors and windows; virtual sources are for remote desktop. The
        // portal draws the dialog.
        //
        // On an X11 session Lightning has its own picker to fall back to, and
        // a portal may advertise ScreenCast without being able to do it:
        // xdg-desktop-portal-kde on X11 (KWin's zkde_screencast is
        // Wayland-only) never produced a session and the button did nothing
        // for minutes (live 2026-10-07). So there the handshake is bounded:
        // no session within kX11PortalPreparationBoundMs, or no answer from
        // its picker within kX11PortalPickerBoundMs, and it is abandoned for
        // Lightning's own picker (the `failed` handler above). Wayland has no
        // fallback and keeps the portal's own, generous timeout.
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
        // No portal on an X11 session: Lightning draws the same picker as on
        // Windows and macOS.
        offerLinuxDisplayPicker(/*autoSelectSingle=*/true);
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
    if (m_state == State::Reconnecting) {
        // Lightning's own picker (Windows, macOS, X11) left open when the
        // reconnect began: refuse before anything is captured.
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

bool SfuCallController::sfuSessionLive() const
{
    // While reconnecting, only the attempt that has joined is a session;
    // before that every SFU report belongs to the one we abandoned.
    return active()
        && (m_state != State::Reconnecting
            || m_reconnectPhase == ReconnectPhase::Joined);
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
        const char *avatar;
        bool local;
        bool micMuted;
        bool hand;
    };
    // Fictional names only; a screenshot must never carry a real account.
    // The same people as the demo's Design Lounge, with their avatars.
    // Cameras are off: the demo has no faces to show, so every tile draws
    // its avatar and only the screen share carries a picture.
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
        // Known state, so tiles draw it rather than the unknown placeholder.
        row.micKnown = true;
        row.micMuted = p.micMuted;
        row.cameraKnown = true;
        row.cameraOn = false;
        rows.append(row);
    }
    if (withScreenShare) {
        // Four people: beside a spotlight the speaker strip is capped at
        // 220 px and scrolls past four bubbles, which a still shows as a
        // bubble cut in half.
        rows.resize(4);
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
    // rebuildModels() is skipped for a demo call, and it is what derives the
    // share rows; without them the stage has nothing to spotlight.
    rebuildShareModel();

    m_micMuted = false;
    m_cameraOn = false;
    // Maya shares, not the local user.
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
    // A state with a reason was announced to the user: every
    // teardown(State::Failed, ...) is followed by callFailed(m_lastError).
    if (!error.isEmpty()) {
        m_failureAnnounced = true;
        m_announcedFailure = error;
    }
    Q_EMIT stateChanged();

    // Withdraw an announced failure once a later attempt gets past the gate
    // that refused it (Authorizing or later: the membership was accepted).
    // An empty callFailed() is the codebase's idiom for clearing an error.
    // Not on Preparing, where the same gate may refuse again. Track-level
    // publish failures are not covered (the camera's has its own withdrawal;
    // see announceCameraNotice()).
    const bool pastTheJoinGate = state == State::Authorizing
        || state == State::Connecting || state == State::Connected
        || state == State::Reconnecting;
    if (hadOutstandingFailure && error.isEmpty() && pastTheJoinGate) {
        m_failureAnnounced = false;
        qCInfo(lcSfuCall) << "the previous call failure no longer applies; "
                             "withdrawing it";
        // Only if it is still the notice showing: a later one (the
        // microphone's, say) is not this one to clear.
        withdrawNotice(m_announcedFailure);
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
    // Chosen applications are never widened to the whole system.
    if (category == QLatin1String("share_audio_apps_unavailable"))
        return tr("Your screen is being shared without its sound — the "
                  "apps you chose can't be captured on this system.");
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
    // A snap opens the camera itself only through its camera interface,
    // which the store does not connect by default.
    if ((category == QLatin1String("camera_failed")
         || category == QLatin1String("camera_no_frames"))
        && sandboxenv::isSnap()) {
        return tr("Your camera isn't available. Lightning is a snap: run "
                  "snap connect lightning:camera");
    }
    if (category == QLatin1String("camera_failed"))
        return tr("Your camera isn't available.");
    // Opened and never sent a frame: a virtual camera with nothing feeding
    // it, or a device node that is not a capture device.
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
    // Never replaced by another camera; see CaptureDeviceSelection.h.
    if (category == QLatin1String("camera_unavailable"))
        return tr("The camera you chose isn't available, so no camera was "
                  "turned on. Choose a camera in Settings.");
    // The device list did not answer (a hung provider), which says nothing
    // about the camera itself.
    if (category == QLatin1String("camera_list_unavailable"))
        // Not "try again": a device list that hung once is not asked again
        // this session (see monitorCandidates), so only a restart, or no
        // stored choice at all, gets past it.
        return tr("Lightning couldn't read the list of cameras, so no camera "
                  "was turned on. Restart Lightning, or choose the system "
                  "default camera in Settings.");
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

void SfuCallController::refuseJoin(const QString &message, bool announce)
{
    // A call already running stays as it is: Failed would read as inactive
    // while its engine and membership live on, and the next join would then
    // skip tearing it down. The refusal is still reported.
    if (active()) {
        qCWarning(lcSfuCall) << "join refused while a call is active; "
                                "keeping that call";
        // Announced, as setState() would record it, so a later join that
        // gets past the gate withdraws it.
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
    // Per call: keys, who holds them and sends awaiting answers belong to the
    // call that made them. m_keyCursor is deliberately kept: see its
    // declaration.
    m_useKeyTimer.stop();
    m_keySwitchFallbackTimer.stop();
    m_unaddressableLogged = -1;
    // Best-effort scrub; the copies handed to the bridge are not zeroed.
    m_newestKey.raw.fill('\0');
    m_newestKey = OutboundKey{};
    m_adoptedKeySerial = 0;
    m_adoptedKeyIndex = -1;
    m_keyRecipients.clear();
    m_keyHolders.clear();
    m_adoptedKeyRecipients.clear();
    m_deliveredKeyIndex = -1;
    m_keySendOps.clear();
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
    stopReconnect();
    m_shareSourceReusable = false;
    m_transportEverConnected = false;
    m_reconnectEpisodeStarts.clear();
    m_roomId = roomId;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    // A new call tries the preferred camera route again.
    m_cameraFellBack = false;
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
    resetMicrophoneLevel();
    setNoiseSuppressionFailedMode(QString());
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
    resetKeyLane();
    m_playbackLostAnnounced = false;
    m_candidatesSent = 0;
    m_lastPublishMs = 0;
    m_membershipAcceptedMs = 0;
    m_membershipLongExpiryUnguarded = false;
    m_refreshOp = 0;
    m_delayedRestartOp = 0;
    supersedeOwedRetractionFor(roomId);

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

namespace {
// Emits membershipWritesSettled() when the function it guards takes
// membershipWritesPending() from true to false, whichever return it leaves by.
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
} // namespace

void SfuCallController::onMembershipPublished(quint64 opId, bool ok,
                                              const QString &category,
                                              const QString &eventId,
                                              const QString &delayId,
                                              const QString &delayedCategory)
{
    const MembershipSettleGuard settle(this);
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
        // Use the delay id from this answer: nothing else holds it. The write
        // just landed, so its expiry window starts now. A refused answer that
        // still names an event is the 4 h write without its short fallback.
        startRetraction(room, delayId, QDateTime::currentMSecsSinceEpoch(),
                        !ok && delayId.isEmpty());
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
            // An event id on a failure is rtc.rs's 4 h write that landed
            // without a delayed retraction or its short replacement: that
            // membership now lives 4 h unless we retract it.
            if (!eventId.isEmpty() && delayId.isEmpty()) {
                m_membershipAcceptedMs = QDateTime::currentMSecsSinceEpoch();
                m_membershipLongExpiryUnguarded = true;
            }
            return;
        }
        m_membershipPublished = true;
        m_membershipAcceptedMs = QDateTime::currentMSecsSinceEpoch();
        // Whatever this write carried replaced any unguarded 4 h one.
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
    // Recorded before the failure branch, whose teardown needs to know
    // whether there is anything to retract; see the event-id note above.
    if (ok || !eventId.isEmpty()) {
        m_membershipPublished = true;
        m_membershipAcceptedMs = QDateTime::currentMSecsSinceEpoch();
        // See the refresh branch: a failure with an event id is an unguarded
        // 4 h membership.
        m_membershipLongExpiryUnguarded = !ok && delayId.isEmpty();
    }
    if (m_state != State::Preparing)
        return; // a reply for a call we already left
    if (!ok) {
        // Logged as well as shown, so the two `forbidden` gates can be told
        // apart (issue #10).
        const QString reported = membershipRefusalCategory(category);
        qCWarning(lcSfuCall)
            << "membership REFUSED by the homeserver category=" << category
            << "reportedAs=" << reported;
        // Captured first: teardown() clears m_roomId.
        const QString refusedRoom = m_roomId;
        teardown(State::Failed, userFacingError(reported));
        Q_EMIT callFailed(m_lastError);
        // The refusal is evidence for the lane gate. Reported after the
        // teardown, so the session change it announces cannot reach a call
        // that is still Preparing, and a lane fallback it triggers starts
        // after this failure was shown.
        if (m_rtc)
            m_rtc->noteMembershipRefused(refusedRoom, category);
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
    // A reconnect has its own reading of every report; see
    // onReconnectSfuState().
    if (m_state == State::Reconnecting) {
        onReconnectSfuState(state, category);
        return;
    }
    // A call that was up and lost its SFU session recovers instead of ending:
    // the SFU asking us back (a Leave with RESUME or RECONNECT, its media
    // timeout), the signalling socket dying ("reconnecting" from the bridge),
    // or a socket that closed without a Leave. A call that never got up
    // keeps failing honestly, below.
    if (m_state == State::Connected
        && (state == QLatin1String("reconnecting")
            || state == QLatin1String("closed")
            || (state == QLatin1String("failed")
                && !sfuFailureIsRefusal(category)))) {
        beginReconnect(category.isEmpty() ? state : category);
        return;
    }
    if (state == QLatin1String("reconnecting")) {
        // Lost before the call was ever up: a failed join, as before.
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
        // The SFU dropped us for good (a Leave with DISCONNECT: removed, room
        // closed), or lost us before the call was up; report it rather than
        // silently ending.
        if (m_state == State::Connected || m_state == State::Connecting) {
            teardown(State::Failed, connectionLostMessage());
            Q_EMIT callFailed(m_lastError);
        }
    }
}

QString SfuCallController::connectionLostMessage() const
{
    // A call is "Connected" from the first SDP, before any media path
    // exists, so only a transport that actually connected makes this a call
    // that was lost rather than one that never got through.
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
    // An answer that will be the same on every attempt: the service or the
    // homeserver refusing us, or our own policy refusing the focus. Anything
    // else is retried within the reconnect budget, deliberately broadly: the
    // focus worked minutes ago, and with the network down even DNS fails
    // (the JWT fetch then reports `invalid_transport`, a captive portal
    // `tls_failed`).
    static const QSet<QString> refusals = {
        QStringLiteral("forbidden"),
        QStringLiteral("sfu_forbidden"),
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
        // Nothing to come back to: end as before.
        if (active()) {
            teardown(State::Failed, connectionLostMessage());
            Q_EMIT callFailed(m_lastError);
        }
        return;
    }
    const bool fresh = m_state != State::Reconnecting;
    if (fresh && !admitReconnectEpisode()) {
        // Lost again and again: whatever the cause (a flapping network, or a
        // focus that keeps asking us back), another round would only repeat
        // it. End honestly.
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
        // The membership, its refresh timer, the participant tiles and every
        // key stay: this is the same call. The UI shows "Reconnecting…".
        setState(State::Reconnecting);
        // A share being chosen cannot be published until the call is back:
        // close the portal's session (consent given later would leave it
        // open) and Lightning's own picker.
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
    // The peer connections go; the keys stay (a peer's key is not sent
    // twice). Captures stop until the rejoin publishes again.
    if (!m_engine.isNull())
        m_engine->suspend();
#endif
    // Sends a Leave if that socket still works, so the SFU drops the old
    // session at once rather than at its own timeout, and bumps the bridge's
    // session so nothing the old one has queued can reach us.
    if (m_client)
        m_client->sfuDisconnect();
    // A camera grant in flight would publish into the suspended engine; the
    // rejoin asks again.
    if (m_cameraAwaitingPortal) {
        m_cameraAwaitingPortal = false;
        if (m_cameraPortal)
            m_cameraPortal->cancel();
    }
    // What the abandoned session published is gone with it; the rejoin
    // declares fresh tracks. The user's intent (camera on, mute, deafen)
    // stays and is re-applied.
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_shareAudioCid.clear();
    resetShareAudioLevel();
    m_publishedTrackIds.clear();
    m_publishedTrackSids.clear();
    m_candidatesSent = 0;
    // A portal share's PipeWire descriptor went down with the engine and
    // cannot be published again without asking the portal: that share ends
    // here, honestly, rather than reappearing as a frozen tile. Window and
    // display shares (Windows, the X11 fallback) are published again on the
    // rejoin from the source they were started with.
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
    // Badges and the meter describe media of the session that is gone.
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
    // 1, 2, 4, 8, 8… s after the loss or the failed attempt.
    int delay = m_reconnectFirstBackoffMs;
    for (int i = 0; i < m_reconnectAttempts && delay < m_reconnectMaxBackoffMs;
         ++i) {
        delay *= 2;
    }
    delay = qMin(delay, m_reconnectMaxBackoffMs);
    // An attempt that cannot start before the budget runs out is not made;
    // the deadline ends the call.
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
    // The same focus and room as the call's membership, which is unchanged:
    // to everyone else this device never left the call.
    const quint64 op = m_client->sfuConnect(m_focusUrl, m_roomId);
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
    // teardown() retracts the membership, which is right now: the call is
    // over for this device.
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
    // Only the current attempt's session may move a reconnect. The bridge
    // already drops reports from abandoned sessions; the phase is the second
    // line: before this attempt's websocket is up, nothing but its own
    // authorization or failure can arrive, so a "closed" or a Leave then is
    // a straggler from the session we left.
    const bool dialing = m_reconnectPhase == ReconnectPhase::Dialing;
    const bool live = m_reconnectPhase == ReconnectPhase::Signalling
        || m_reconnectPhase == ReconnectPhase::Joined;
    if (state == QLatin1String("authorized")) {
        return; // still dialing
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
            // The service refused us; it will refuse every attempt.
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
            // The new session was told to go for good (DISCONNECT).
            giveUpReconnect(connectionLostMessage());
            return;
        }
        reconnectAttemptFailed(category.isEmpty() ? state : category);
        return;
    }
}

void SfuCallController::onEngineTransportState(int target,
                                               const QString &state)
{
    qCInfo(lcSfuCall) << "transport target=" << target << "state=" << state
                      << "callState=" << static_cast<int>(m_state);
    if (!active())
        return;
    if (state == QLatin1String("connected")) {
        m_iceDisconnectedTimer.stop();
        m_transportEverConnected = true;
        // The new session's media path is up: that, not the join, ends a
        // reconnect.
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
    // A rejoin after a lost session: only the current attempt's join counts.
    // Before its websocket was up, or after its join, a JoinResponse is a
    // straggler from another session (the bridge filters those too).
    const bool rejoin = m_state == State::Reconnecting;
    if (rejoin && m_reconnectPhase != ReconnectPhase::Dialing
        && m_reconnectPhase != ReconnectPhase::Signalling) {
        qCInfo(lcSfuCall) << "stale sfu join ignored while reconnecting";
        return;
    }
    // Outside the media guard, so the reconnect state machine is the same
    // in a build without an engine (where join() refuses anyway).
    m_ownIdentity = identity;
    // Seed the remote mute record from the join's participant list.
    noteRemoteTrackMutes(participants);
    // On a rejoin this replaces the list the lost session left: the same
    // people, and our own row under the same identity with a new sid.
    m_participants = participants.mid(0, kMaxParticipants);
    noteParticipantIdentities();
    rebuildModels();
    if (rejoin) {
        m_reconnectPhase = ReconnectPhase::Joined;
        // Everyone present is sent our current key again: a receiver that
        // dropped it while we were gone (or that a send never reached) can
        // decrypt us from the first frame. The recipients are kept, so this
        // is a re-send to the same devices, never a rotation, and still only
        // to SFU participants the membership names (mediaKeyTargets()).
        m_keyHolders.clear();
        qCInfo(lcSfuCall) << "reconnect attempt" << m_reconnectAttempts
                          << "joined inCall=" << m_participants.size()
                          << "elapsedMs=" << m_reconnectClock.elapsed();
    }
#ifdef HAVE_LIGHTNING_WEBRTC
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
        // A window or display share continues from the source it was
        // started with; see suspendSfuSession() for a portal share.
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
    // A rejoin stays Reconnecting until the new transport is up
    // (onEngineTransportState).
    if (!rejoin)
        setState(State::Connecting);
    else
        distributeKeyIfNeeded();
    // Apply keys a peer sent before we got here.
    applyParkedKeys();
    Q_EMIT participantsChanged();
    // The key is minted in publishTracks(), before the first frame.
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
        startKeyLane();
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
    if (!sfuSessionLive())
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
    // A leaver makes us rotate, a joiner is sent our key; see
    // distributeKeyIfNeeded(). Every update, not only a changed identity set:
    // a rejoin can replace a row under the same identity with a new sid.
    distributeKeyIfNeeded();
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
    // and a leave.
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
    if (!sfuSessionLive())
        return;
    mergeSpeakers(speakers);
}

void SfuCallController::mergeSpeakers(const QVariantList &speakers)
{
    // SpeakersChanged is a DELTA, as livekit-client reads it: the server sends
    // a speaker when it starts or its level moves, and one that stopped with
    // active=false. A steady speaker is not repeated, so replacing the set
    // with each message rang only whoever changed last.
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
        // LiveKit's SpeakerInfo `level` (0..1). Absent stays absent; the model
        // treats it as 0.0 rather than inventing an amplitude.
        if (entry.contains(QStringLiteral("level"))) {
            m_speakingLevel.insert(
                sid, entry.value(QStringLiteral("level")).toDouble());
        } else {
            m_speakingLevel.remove(sid);
        }
    }
    // Per-row dataChanged on the speaking roles only; participantsChanged()
    // would rebuild every tile and VideoOutput on every syllable.
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
        || category == QLatin1String("share_audio_unavailable")
        || category == QLatin1String("share_audio_apps_unavailable");
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
        resetShareAudioLevel();
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

void SfuCallController::onRemotePlaybackFailed(bool failed)
{
    if (!active())
        return;
    if (failed == m_playbackLostAnnounced)
        return;
    m_playbackLostAnnounced = failed;
    qCWarning(lcSfuCall) << "received audio output failed=" << failed;
    const QString notice = playbackLostNotice();
    // callFailed reports without ending the call; the withdrawal once the
    // engine got the output back clears only this notice.
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
    m_cameraNotice = notice;
    m_cameraNoticeCid = cid;
}

void SfuCallController::onEnginePublishFailed(const QString &cid,
                                              const QString &category)
{
    // Only connected when there is an engine. Outside the media guard, except
    // the engine's own calls, so tests without an engine reach the notices.
    qCWarning(lcSfuCall) << "publish failed category=" << category
                         << "camera=" << (cid == m_cameraCid)
                         << "screen=" << (cid == m_screenCid);
    if (!active() || cid.isEmpty())
        return;
    // Turn the control back off and withdraw the track; the call stays up.
    const bool camera = cid == m_cameraCid;
    QString noticeCategory = category;
    if (camera) {
        const bool viaPortal = m_cameraViaPortal;
        m_cameraOn = false;
        unpublishTrack(m_cameraCid);
#ifdef HAVE_LIGHTNING_WEBRTC
        clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
#endif
        // A portal camera that gave nothing gets one retry on the device
        // node the user granted; see cameraFailureStep().
        if (cameraFailureStep(category, viaPortal, cameraDeviceNodeVisible(),
                              m_cameraFellBack)
            == CameraFailureStep::OpenDeviceNode) {
            fallBackToDeviceCamera(category);
            return;
        }
        noticeCategory = cameraNoticeCategory(category, viaPortal,
                                              m_cameraPortalReportedCamera);
    } else if (cid == m_screenCid) {
        m_screenSharing = false;
        unpublishTrack(m_screenCid);
        // The sound goes with the picture, as in stopScreenShare(): a share
        // that failed must not keep broadcasting the desktop's audio.
        if (!m_shareAudioCid.isEmpty())
            unpublishTrack(m_shareAudioCid);
        shareAudioShareEnded();
        if (m_portal)
            m_portal->cancel();
#ifdef HAVE_LIGHTNING_WEBRTC
        clearLocalVideoSurface(SfuMediaEngine::localScreenStreamId());
#endif
    } else if (cid == m_audioCid) {
        // The engine gave up restarting the microphone. The track stays: the
        // user can still hear everyone, and is told nobody hears them.
        Q_EMIT callFailed(userFacingError(category));
        return;
    } else {
        // A track we no longer own; nothing to do.
        return;
    }
    applyVideoState();
    Q_EMIT mediaStateChanged();
    // A plain-wording notice; the state is unchanged and the call stays
    // active. The camera's is withdrawn by a later camera that works.
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
#ifdef LIGHTNING_ENABLE_SCREENSHOT_DEMO
    // A staged demo call has no media engine: show a bundled still as the
    // shared screen so the stage is not an empty frame.
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

QList<SfuCallController::KeyTarget> SfuCallController::mediaKeyTargets() const
{
    // Targets are the devices actually in the call: the SFU participant list
    // (presence) intersected with the membership (identity -> Matrix device).
    // Membership alone includes ghosts left by clients that died without
    // retracting, which receive the key while the live peer gets nothing.
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
        // Skip our own device and any identity not yet resolved; the next
        // update or membership read retries.
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
    // The unit separator cannot occur in either half.
    return target.userId + QChar(0x1f) + target.deviceId;
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
    // Nothing to encrypt with. Without WebRTC join() refuses, so this is only
    // reached by tests, which run the policy without an engine.
    if (m_engine.isNull())
        return false;
#endif
    return true;
}

void SfuCallController::startKeyLane()
{
    if (!keyLaneReady())
        return;
    // One first key per call; a second call here only reconciles.
    if (m_newestKey.serial != 0) {
        distributeKeyIfNeeded();
        return;
    }
    // Used at once, since nothing is in use yet (matrix-js-sdk's first key
    // too).
    rotateMediaKey(mediaKeyTargets(), "first key");
}

void SfuCallController::distributeKeyIfNeeded()
{
    if (!keyLaneReady())
        return;
    // The first key is minted with the tracks (startKeyLane()); until then
    // there is nothing to send.
    if (m_newestKey.serial == 0)
        return;
    const QList<KeyTarget> targets = mediaKeyTargets();
    QSet<QString> present;
    for (const KeyTarget &t : targets)
        present.insert(keyTargetDevice(t));

    if (targets.isEmpty()) {
        const int sfuPeers = sfuPeerCount();
        // A withheld key (a newer key than the one a peer took) also triggers
        // a server refresh: the SFU peer count can be wrong.
        const bool keyWithheld = m_deliveredKeyIndex >= 0
            && m_newestKey.index != m_deliveredKeyIndex;
        // Runs on every update and tick: logged when what it says changes.
        const int logged = sfuPeers * 2 + (keyWithheld ? 1 : 0);
        if (logged != m_unaddressableLogged) {
            m_unaddressableLogged = logged;
            qCInfo(lcSfuCall) << "media key: nobody addressable"
                              << "sfuPeers=" << sfuPeers
                              << "keyWithheld=" << keyWithheld;
        }
        // Peers exist but none can be named: refresh from the server.
        if (sfuPeers > 0 || keyWithheld)
            m_rtc->refreshFromServer(m_roomId);
    } else {
        m_unaddressableLogged = -1;
    }

    // A leaver: a device the newest key went to that is no longer here.
    int left = 0;
    for (auto it = m_keyRecipients.cbegin(); it != m_keyRecipients.cend();
         ++it) {
        if (!present.contains(it.key()))
            ++left;
    }
    // A joiner: new to the newest key, or back with a new SFU session (it
    // rejoined and lost our key). Anyone present without the key (a joiner,
    // or a send reported failed) is sent it.
    int joined = 0;
    QList<KeyTarget> needKey;
    for (const KeyTarget &t : targets) {
        const QString device = keyTargetDevice(t);
        const auto r = m_keyRecipients.constFind(device);
        const bool isNew = r == m_keyRecipients.cend() || r.value() != t.session;
        if (isNew)
            ++joined;
        if (isNew || !m_keyHolders.contains(device))
            needKey.append(t);
    }

    const bool switchPending = keySwitchPending();
    if (left > 0 && !switchPending) {
        rotateMediaKey(targets, "a holder left");
        return;
    }
    if (left > 0) {
        // One switch at a time, as matrix-js-sdk runs one rollout at a time:
        // adoptNewestKey() reconciles again once it has switched.
        qCInfo(lcSfuCall) << "media key: a holder left during a pending"
                          << "switch; rotating after it left=" << left;
    }
    // A joiner gets a fresh key when ours is old, so it cannot decrypt more
    // than the grace period of what we sent before it arrived (if it can get
    // that ciphertext at all: only the SFU has it).
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
    // Every update and tick; debug only.
    if (!targets.isEmpty())
        qCDebug(lcSfuCall) << "media key: no redistribution, set unchanged";
}

void SfuCallController::rotateMediaKey(const QList<KeyTarget> &targets,
                                       const char *reason)
{
    // Decided against the key in use and who holds it, before either moves.
    const bool first = (m_adoptedKeySerial == 0);
    const bool switchNow = first || !someonePresentHoldsAdoptedKey(targets);

    // 32 bytes from the system CSPRNG; never the generic PRNG for keys.
    QByteArray key(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32 *>(key.data()),
        reinterpret_cast<quint32 *>(key.data() + key.size()));
    // The cursor advances for every key minted, sent or not, and skips the
    // index our frames use: a key sent there would replace, at every
    // receiver, the one our frames still need. Reachable after 16 keys
    // minted without a switch (a withheld key retried).
    m_keyCursor = (m_keyCursor + 1) % kSendKeyIndices;
    if (m_adoptedKeySerial != 0 && m_keyCursor == m_adoptedKeyIndex)
        m_keyCursor = (m_keyCursor + 1) % kSendKeyIndices;

    m_useKeyTimer.stop();
    m_keySwitchFallbackTimer.stop();
    // Best-effort scrub of the key being replaced; the engine keeps its own
    // derived copy of a key in use.
    m_newestKey.raw.fill('\0');
    m_newestKey.index = m_keyCursor;
    m_newestKey.raw = std::move(key);
    m_newestKey.mintedMs = m_keyClock.elapsed();
    m_newestKey.serial = ++m_keySerialCounter;
    m_keyRecipients.clear();
    m_keyHolders.clear();

    const int sfuPeers = sfuPeerCount();
    // Both counts: targets=0 with and without SFU peers are different
    // defects. Counts only.
    qCInfo(lcSfuCall) << "media key distributed index=" << m_newestKey.index
                      << "targets=" << targets.size()
                      << "sfuPeers=" << sfuPeers
                      << "unresolved=" << (sfuPeers - targets.size())
                      << "reason=" << reason;
    // Which devices, as user/device pairs (the same class of identifier as the
    // receive side's `ring=` line), so both ends' logs can be compared.
    {
        QStringList named;
        named.reserve(targets.size());
        for (const KeyTarget &t : targets)
            named << (t.userId + QLatin1Char('/') + t.deviceId);
        qCInfo(lcSfuCall) << "media key targeted devices="
                          << (named.isEmpty() ? QStringLiteral("<none>")
                                              : named.join(QLatin1String(", ")));
    }

    // Sent first, installed second: our frames are never encrypted under a
    // key a present peer cannot have yet.
    sendNewestKeyTo(targets);
    if (switchNow) {
        // Nobody present decrypts with the key in use, so switching now cuts
        // nobody off: the first key of a call, the last holder gone, or a
        // key that never reached anyone.
        installNewestKey();
        return;
    }
    // Keep encrypting under the key in use until the peers hold this one.
    // Switching at once drops our media at every peer for the to-device
    // delivery time (measured up to 4.7 s). The switch comes kUseKeyDelayMs
    // after the send to the holders of the key in use is answered
    // (startUseKeyDelay()), and never later than kKeySwitchFallbackMs after
    // the dispatch, answered or not.
    //
    // Security: a holder that just left can decrypt our frames until that
    // switch, at most kKeySwitchFallbackMs after we saw it leave (twice that
    // when another holder leaves meanwhile; see distributeKeyIfNeeded()), but
    // only while the new key reaches someone. When its send is refused, every
    // send of it is reported failed, or the only peers left are SFU
    // participants we cannot name, a key that reached nobody is never
    // switched to while a peer may hold the one in use (adoptNewestKey()),
    // so our frames stay on the key the leaver holds until a later send
    // succeeds. matrix-js-sdk accepts the same window (`useKeyDelay` after
    // `await sendKey`) and keeps the old key when sendKey throws.
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
    // op 0: the Rust side refused to dispatch and no answer will come.
    // Nobody is recorded as holding it, so the next reconciliation retries.
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
        // Checked before the insert below: whether this device held the key
        // in use before this send.
        if (m_adoptedKeyRecipients.contains(device))
            send.reachesKeyInUse = true;
        m_keyRecipients.insert(device, t.session);
        m_keyHolders.insert(device, t.session);
        if (inUse)
            m_adoptedKeyRecipients.insert(device);
    }
    m_keySendOps.insert(op, send);
    // A key not in use yet: the switch follows this send's answer (see
    // startUseKeyDelay()), and the fallback armed here bounds it when the
    // answer is late or never comes. A switch already pending keeps its time,
    // so churn cannot postpone it.
    if (!inUse && !keySwitchPending())
        m_keySwitchFallbackTimer.start();
}

void SfuCallController::startUseKeyDelay(quint64 serial, bool reachesKeyInUse)
{
    // Only for the newest key, not in use yet, and once: an answer about an
    // older key says nothing about this one, and a later answer must not
    // postpone the switch.
    if (serial == 0 || serial != m_newestKey.serial
        || m_newestKey.serial == m_adoptedKeySerial
        || m_useKeyTimer.isActive()) {
        return;
    }
    // Only a send that went to a holder of the key in use says the holders
    // can have this one: sends run concurrently, so a joiner's quick answer
    // must not start the switch while the send to a slower holder is still
    // in flight. With no holder we can name present, no answer can say
    // more, and any answer counts.
    if (!reachesKeyInUse) {
        for (const KeyTarget &t : mediaKeyTargets()) {
            if (m_adoptedKeyRecipients.contains(keyTargetDevice(t)))
                return;
        }
    }
    // Counted from the answer, as matrix-js-sdk sleeps after `await sendKey`:
    // the time our send spent before its PUT (a /keys/query per due user,
    // one-time-key claims) must not come out of the delay. The fallback stays
    // armed, so the switch is never later than it.
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
        // Withheld, not pending: the next send re-arms the switch.
        m_useKeyTimer.stop();
        m_keySwitchFallbackTimer.stop();
        return;
    }
    installNewestKey();
    qCInfo(lcSfuCall) << "media key switched to index=" << m_newestKey.index;
    // A holder that left while the switch was pending is rotated for now.
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
    // The key in use never reached anyone.
    if (m_adoptedKeyRecipients.isEmpty())
        return false;
    for (const KeyTarget &t : targets) {
        if (m_adoptedKeyRecipients.contains(keyTargetDevice(t)))
            return true;
    }
    // An SFU peer we cannot name may be one of them.
    return sfuPeerCount() > targets.size();
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
    // Same for a publish parked by teardown().
    m_abandonedPublishOp = 0;
    m_abandonedPublishRoomId.clear();
    if (wasPending)
        Q_EMIT membershipWritesSettled();
}

bool SfuCallController::membershipWritesPending() const
{
    // The room id is held from dispatch until success, giving up, or a
    // failed dispatch, including while a retry waits on its timer.
    return m_retractOp != 0 || !m_retractRoomId.isEmpty()
        || m_abandonedPublishOp != 0;
}

void SfuCallController::onMembershipRetracted(quint64 opId, bool ok,
                                              const QString &category)
{
    const MembershipSettleGuard settle(this);
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
    if (m_supersededRetractOp != 0 && opId == m_supersededRetractOp) {
        const QString room = m_supersededRetractRoomId;
        m_supersededRetractOp = 0;
        m_supersededRetractRoomId.clear();
        // The previous call's retraction, still in flight when we rejoined
        // this room. Landing after our new publish, it removed the membership
        // of the call we are in (one state key per device); put it back.
        if (!ok || !active() || room != m_roomId)
            return;
        // Only where a publish arms no second delayed retraction. Every
        // publish may arm one (rtc.rs) and nothing cancels the previous, so
        // with one armed (m_delayId) or a publish in flight that may arm
        // one, the orphan would fire `{}` on our state key 8 s later and
        // remove the membership this was meant to restore. Those cases are
        // left as before this repair existed; without MSC4140 the 60 s
        // heartbeat re-publish restores the membership in any case.
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
    // Retry only transient failures; anything else would fail the same way.
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
    // A transient failure keeps the retraction owed until the membership has
    // expired for every reader. An attempt count is the wrong bound: the
    // failure is usually the network itself, and it outlasts any short chain
    // (a 75 s outage outlived the old four attempts by a minute).
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
        // The homeserver answered while this attempt was in flight, so the
        // outage is over; do not sit out the full backoff.
        m_retractRetryPromptly = false;
    } else {
        const int doublings = qBound(0, m_retractAttempts - 1, 10);
        delay = qMin<qint64>(qint64(kRetractRetryDelayMs) << doublings,
                             kRetractRetryMaxDelayMs);
    }
    // The last attempt lands at the deadline, not after it.
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
        // An attempt is in flight, possibly sent into the outage and still
        // waiting out its timeout. Its failure retries promptly.
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
    // The account check is the same promise abandonOutstandingMembershipWrites()
    // keeps, made again at the moment of sending, since an owed retraction
    // now waits minutes rather than seconds: never under another account.
    const QString current = m_client ? m_client->currentUserId() : QString();
    const QString device = m_client ? m_client->currentDeviceId() : QString();
    // The device too: the state key is per device, and a re-login of the
    // same account is a new device whose membership this is not.
    if (current != m_retractUserId || device != m_retractDeviceId) {
        qCWarning(lcSfuCall)
            << "an owed call retraction belongs to an account that is no "
               "longer the client's; dropped, the membership is the server's "
               "to expire";
        clearOwedRetraction();
        return false;
    }
    // A timer armed by the failure path fires at the deadline at the latest,
    // and that final attempt is still sent; only a later trigger is refused.
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
    // Supersedes a retraction still owed to another room (one slot). That
    // one was offered to the server at least once, and its membership
    // expires on its own.
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
    // One attempt at a time; its answer arms the next.
    m_retractRetryTimer.stop();
    ++m_retractAttempts;
    m_retractOp =
        m_client->rtcRetractMembership(m_retractRoomId, m_retractDelayId);
    if (m_retractOp != 0) {
        m_retractAnswerTimer.start(m_retractAnswerTimeoutMs);
        return;
    }
    // Not dispatched, so no answer will arrive; do not wait for one.
    qCWarning(lcSfuCall)
        << "retraction could not be dispatched — this device will remain in "
           "the room's call membership until it expires";
    clearOwedRetraction();
}

void SfuCallController::supersedeOwedRetractionFor(const QString &roomId)
{
    // A retraction still owed for this room is moot: we are re-joining, and
    // a retry would remove the membership about to be created. An attempt
    // already in flight cannot be recalled, so its op id is kept: if it is
    // accepted after our new publish, onMembershipRetracted() re-publishes
    // where no second delayed retraction can be armed.
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
    // Always logged, so a call's end always has a visible reason.
    qCInfo(lcSfuCall) << "teardown state=" << static_cast<int>(finalState)
                      << "error=" << (error.isEmpty()
                                      ? QStringLiteral("<none>") : error);
    ++m_generation;
    // A Leave pressed while reconnecting ends here, at once: no attempt is
    // launched after this and none in flight can revive the call.
    stopReconnect();
    m_shareSourceReusable = false;
    m_transportEverConnected = false;
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
    resetKeyLane();
    // "Leave and rejoin" is advice about this call; once it has ended the
    // notice is withdrawn (only if it is still the one showing).
    if (m_playbackLostAnnounced)
        withdrawNotice(playbackLostNotice());
    m_playbackLostAnnounced = false;
    // The same for the other in-call notices: "Your microphone isn't
    // available" and a camera's notice described THIS call's devices, and
    // stayed on the status bar after leaving it (measured in the Flatpak).
    withdrawNotice(userFacingError(QStringLiteral("audio_source_failed")));
    if (withdrawNotice(m_cameraNotice)) {
        m_cameraNotice.clear();
        m_cameraNoticeCid.clear();
    }

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
            // MSC4140, or leave during Preparing). Tracked and retried until
            // the membership would have expired anyway, which is counted
            // from the last write the server accepted.
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
    resetMicrophoneLevel();
    setNoiseSuppressionFailedMode(QString());
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
    // The engine and its share bin are gone; a stale id would stop the next
    // call's share from publishing its sound (publishShareAudioTrack()).
    m_shareAudioCid.clear();
    m_cameraOn = false;
    // Per call: the next call must not skip the portal on this one's account.
    m_cameraFellBack = false;
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

void SfuCallController::noteMicrophoneLevelAt(double peakDb, qint64 nowMs)
{
    // A muted capture posts nothing (`level` is after the valve), but a
    // reading already queued when the user muted must not light the meter.
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
    // Muted (or deafened, which mutes) shows an empty meter at once rather
    // than the last word spoken.
    if (m_micMuted)
        resetMicrophoneLevel();
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
    // Before publishAudio() builds the chain, and live afterwards.
    m_engine->setNoiseSuppressionMode(
        m_settings ? calls::noise::modeFromKey(
                         m_settings->noiseSuppressionMode().toStdString(),
                         calls::noise::kDefaultMode)
                   : calls::noise::kDefaultMode);
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
    const bool deviceNode = cameraDeviceNodeVisible();
    const bool portalWired = !m_cameraPortal.isNull();
    const bool portalUsable = portalWired && CameraPortal::available()
        && CameraPortal::cameraPresent();
    LinuxCameraRoute route =
        linuxCameraRoute(sandboxed, portalUsable, deviceNode);
    // The portal already gave this camera nothing (see
    // fallBackToDeviceCamera()): a rejoin goes straight to the device node
    // rather than making the user wait for the same silence again.
    if (route == LinuxCameraRoute::Portal && m_cameraFellBack && deviceNode) {
        qCInfo(lcSfuCall) << "camera portal gave no picture earlier in this "
                             "call; using the device node";
        route = LinuxCameraRoute::Direct;
    }
    // The portal route needs a wired portal object, or a sandboxed build would
    // wait forever on a signal nothing can emit.
    // Except in a sandbox with no device node, where the direct device cannot
    // exist either: say what is missing instead (below).
    const bool nothingToOpen = sandboxed && !deviceNode;
    if (route == LinuxCameraRoute::Portal && !portalWired && !nothingToOpen) {
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

    m_cameraViaPortal = route == LinuxCameraRoute::Portal;
    m_cameraPortalReportedCamera = portalUsable;
    if (route == LinuxCameraRoute::Portal && !portalWired) {
        // A sandbox with neither a camera portal nor a device node (Flathub's
        // permissions on a desktop without xdg-desktop-portal): opening
        // v4l2src would only fail with "Your camera isn't available".
        qCWarning(lcSfuCall) << "no camera portal and no device node in the "
                                "sandbox; no camera can be opened";
        abandonPendingCamera();
        announceCameraNotice(QStringLiteral("camera_portal_unavailable"),
                             QString());
        return;
    }
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
    m_cameraViaPortal = pipewireFd >= 0;
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
    if (m_state == State::Reconnecting) {
        // No session to publish into: record the intent, which the rejoin
        // honours (publishTracks()).
        m_cameraOn = on;
        if (!on)
            clearLocalVideoSurface(SfuMediaEngine::localCameraStreamId());
        Q_EMIT mediaStateChanged();
        return;
    }
    m_cameraOn = on;
    if (on) {
        // A fresh press tries the preferred route again.
        m_cameraFellBack = false;
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
    // No session to publish into while reconnecting (the caller closes the
    // descriptor on a refusal).
    if (!sfuSessionLive())
        return false;
    // Without a source (node id, window handle or X11 rectangle) refuse rather
    // than guess, or the wrong screen gets published. This guard has a twin in
    // SfuMediaEngine::publishVideo; keep them in sync.
    if (pipewireNodeId < 0 && windowHandle == 0 && !captureRect.isValid())
        return false;
    if (m_screenSharing)
        stopScreenShare();
    // Remember the source when it can be captured again without asking
    // anyone, so a reconnect can resume the share. A portal share hands over
    // a PipeWire descriptor that the engine consumes; it cannot.
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
    // how Element renders it and lets it mute and stop independently. Only
    // published when something is chosen and a capture is available.
    publishShareAudioTrack();
    updateShareAudioAppsPolling();
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

void SfuCallController::publishShareAudioTrack()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || !m_shareAudioCid.isEmpty() || m_engine.isNull()
        || !m_client || !sfuSessionLive())
        return;
    if (m_shareAudioMode == 0 || !SfuMediaEngine::shareAudioAvailable())
        return;
    lightning::shareaudio::Selection selection;
    selection.mode = lightning::shareaudio::modeFromInt(m_shareAudioMode);
    selection.keys = m_shareAudioApps;
    if (!selection.capturesAnything()) {
        // Applications mode with none chosen: no track at all, and the status
        // line says so. A track that can only ever carry silence would show
        // the far end a share with sound that has none.
        qCInfo(lcSfuCall) << "screen share audio not published: no "
                             "application chosen";
        return;
    }
    const QString audioCid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qCInfo(lcSfuCall) << "screen share audio publishing encrypted="
                      << m_roomEncrypted << "mode=" << m_shareAudioMode
                      << "chosen=" << m_shareAudioApps.size();
    // kind=0 (audio) with screen_share=true maps to SCREEN_SHARE_AUDIO.
    m_client->sfuAddTrack(audioCid, QStringLiteral("screenaudio"),
                          /*kind=*/0, 0, 0,
                          /*screenShare=*/true, m_roomEncrypted);
    // Record the cid before publishing: publishShareAudio() can emit
    // failed() synchronously, re-entering onEngineFailed, whose cleanup
    // keys on this cid.
    m_shareAudioCid = audioCid;
    m_publishedTrackIds.append(audioCid);
    resetShareAudioLevel();
    m_engine->publishShareAudio(audioCid, selection);
    Q_EMIT shareAudioStatusChanged();
#endif
}

void SfuCallController::applyShareAudioToRunningShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || m_engine.isNull() || !m_client)
        return;
    // While reconnecting there is no session; the resumed share reads the
    // current choice when it publishes.
    if (!sfuSessionLive())
        return;
    lightning::shareaudio::Selection selection;
    selection.mode = lightning::shareaudio::modeFromInt(m_shareAudioMode);
    selection.keys = m_shareAudioApps;
    if (!selection.capturesAnything()
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
    // The running capture cannot express the new choice (one loopback asked
    // for single applications, or the reverse): a new track replaces it.
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
    // The engine reports a new track's state; until then there is none.
    m_shareAudioCarried.clear();
    m_shareAudioFailed.clear();
    m_shareAudioLimitReached = false;
    m_shareAudioReportLive = false;
    m_shareAudioReportPerApp = false;
    m_shareAudioReportExcludesUs = false;
    if (changed)
        Q_EMIT shareAudioStatusChanged();
}

void SfuCallController::shareAudioShareEnded()
{
    resetShareAudioLevel();
    if (m_shareAudioRestorePending) {
        // The picker chose this window's application for the share that just
        // ended; the choice the user had before comes back.
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
    // "On" returns to whatever was chosen before "off".
    setShareAudioMode(on ? (m_shareAudioLastOnMode != 0 ? m_shareAudioLastOnMode
                                                         : 1)
                         : 0);
}

void SfuCallController::setShareAudioMode(int mode)
{
    // An enum, not a quantity: anything unknown is Off (see modeFromInt), and
    // "applications" where none can be chosen is refused rather than widened.
    if (mode != 0 && mode != 1 && mode != 2)
        mode = 0;
    if (mode == 2 && !shareAudioCanChooseApps())
        return;
    if (m_shareAudioMode == mode)
        return;
    m_shareAudioMode = mode;
    if (mode != 0)
        m_shareAudioLastOnMode = mode;
    // A choice made after the picker's one-share preselection is the user's
    // own: ending the share must not put the earlier one back (a "No sound"
    // picked mid-share would otherwise become "Entire system" again).
    m_shareAudioRestorePending = false;
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
        // Choosing an application is choosing "only these applications".
        m_shareAudioMode = 2;
        m_shareAudioLastOnMode = 2;
    }
    // The user's own choice; see setShareAudioMode().
    m_shareAudioRestorePending = false;
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
    if (mode == 2 && !shareAudioCanChooseApps())
        mode = 1;
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
    // Windows excludes our process tree with a single loopback; it used to
    // read per-application capture here, which is never available there, so
    // the picker told Windows users the call would echo when it would not.
    return SfuMediaEngine::shareAudioSystemExcludesUs();
#else
    return false;
#endif
}

bool SfuCallController::shareAudioCanChooseApps() const
{
    if (m_shareAudioCanChooseForTest >= 0)
        return m_shareAudioCanChooseForTest == 1;
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
    // Live while a surface shows the list, or while a share carries chosen
    // applications (their "in the share" marks). Not for the whole system:
    // nothing on screen reads the list then, and on Windows a pass is a COM
    // walk over every endpoint plus a process snapshot.
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
    // On a worker: the GUI thread never waits on PipeWire or COM. One pass at
    // a time; one that hangs is said once.
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
            // Listed by the audio server (a placeholder for a chosen app is not).
            { QStringLiteral("session"), true },
            { QStringLiteral("chosen"), m_shareAudioApps.contains(key) },
            { QStringLiteral("carried"), m_shareAudioCarried.contains(key) },
            { QStringLiteral("failed"), m_shareAudioFailed.contains(key) },
            // Lightning runs inside it (Windows: an ancestor process such
            // as explorer.exe): capturing its tree would capture the call.
            { QStringLiteral("containsUs"),
              a.value(QStringLiteral("containsUs")) },
        });
    }
    // A chosen application that is not playing stays listed (and chosen), so
    // a choice never silently disappears from the list.
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
    in.mode = m_shareAudioMode;
    for (const QString &key : m_shareAudioApps)
        in.chosenLabels.append(m_shareAudioAppLabels.value(key, key));
    // During a share, what the running track ACTUALLY does: it can have
    // fallen back to the output monitor (the echo) while the capability says
    // otherwise. Before one, what the whole system would do here.
    in.trackLive = m_screenSharing && !m_shareAudioCid.isEmpty()
        && m_shareAudioReportLive;
    in.systemExcludesUs = in.trackLive ? m_shareAudioReportExcludesUs
                                       : shareAudioExcludesOwnPlayback();
    in.perApplication = in.trackLive && m_shareAudioReportPerApp;
    in.carriedAny = !m_shareAudioCarried.isEmpty();
    in.levelKnown = m_shareAudioLevelKnown;
    in.heard = m_shareAudioHeard;
    for (const QString &key : m_shareAudioFailed)
        in.failedLabels.append(m_shareAudioAppLabels.value(key, key));
    in.limitReached = m_shareAudioLimitReached;
    return shareAudioStatusText(in);
}

QString SfuCallController::shareAudioStatusText(const ShareAudioStatusInput &in)
{
    if (!in.supported)
        return tr("This system can't capture the sound it plays.");
    QString what;
    switch (in.mode) {
    case 1:
        what = in.systemExcludesUs
            ? tr("Sound: everything this computer plays, except Lightning.")
            : tr("Sound: everything this computer plays, including this call, "
                 "so others may hear themselves.");
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
    // While sharing: say when something is not reaching the far end, which
    // is what a person on the other side would otherwise have to report.
    QStringList notes;
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
    // Always stop both halves: share audio outliving the picture would keep
    // broadcasting the desktop's sound.
    if (!m_shareAudioCid.isEmpty()) {
        unpublishTrack(m_shareAudioCid);
        m_shareAudioCid.clear();
    }
    m_screenSharing = false;
    shareAudioShareEnded();
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
    // A participant who left while speaking gets no active=false; forget it.
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
    supersedeOwedRetractionFor(roomId);
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
