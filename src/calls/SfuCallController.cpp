#include "calls/SfuCallController.h"

#include <QLoggingCategory>

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
#include "matrix/MatrixClient.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include <QVideoSink>

#include "calls/CallFrameCryptor.h"
#include "calls/SfuMediaEngine.h"
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

/// Membership claims four hours of validity; refresh well inside that, and
/// often enough that the MSC4140 delayed retraction (8 s) keeps being
/// restarted. This is the heartbeat that says "still here".
constexpr int kRefreshIntervalMs = 5000;
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
}

SfuCallController::~SfuCallController()
{
    // Never leave a microphone live because an object went away.
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
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::rtcMembershipPublished, this,
            &SfuCallController::onMembershipPublished);
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
#else
    Q_UNUSED(engine);
#endif
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

void SfuCallController::requestScreenShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || m_engine.isNull())
        return;
    if (m_portal.isNull() || !ScreenCastPortal::available()) {
        qCWarning(lcSfuCall) << "screen share unavailable portal="
                             << !m_portal.isNull();
        Q_EMIT callFailed(
            tr("Screen sharing isn't available on this desktop."));
        return;
    }
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
    // Monitors and windows. Virtual sources are deliberately not offered:
    // they exist for remote-desktop use and would confuse the picker here.
    m_portal->requestShare(ScreenCastPortal::Monitor
                           | ScreenCastPortal::Window);
#endif
}

bool SfuCallController::active() const
{
    return m_state != State::Idle && m_state != State::Ended
        && m_state != State::Failed;
}

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
    if (category.startsWith(QLatin1String("screen_share")))
        return tr("Screen sharing couldn't start.");
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
    if (opId == 0 || opId != m_publishOp)
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
    // Empty means the server has no MSC4140: cleanup then relies on the
    // membership's own `expires`, which is honest but slower.
    m_delayId = delayId;
    m_refreshTimer.start();

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
    syncMicMuteToSfu();
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
            if (!sid.isEmpty())
                return sid;
        }
        return {};
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
    // Restart the delayed retraction so the server keeps not firing it, and
    // refresh the membership itself so `expires` stays ahead of now.
    if (!m_delayId.isEmpty())
        m_client->rtcRestartDelayedLeave(m_delayId);
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
            m_client->rtcRetractMembership(m_roomId, m_delayId);
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
    // Local muting stops packets; it does not tell anyone. Other clients read
    // the mute state off the TRACK, so without this the mic icon in Element
    // never moved — and a mute the SFU inferred from silence stayed set after
    // we started sending again, which is the reported "I unmute and remain
    // muted in Element".
    syncMicMuteToSfu();
#endif
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
    }
    Q_EMIT mediaStateChanged();
#else
    Q_UNUSED(on);
#endif
}

void SfuCallController::toggleCamera() { setCameraOn(!m_cameraOn); }

bool SfuCallController::startScreenShare(int pipewireNodeId, int pipewireFd)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || m_engine.isNull() || !m_client)
        return false;
    // A negative node id is REFUSED, never defaulted: the portal is what
    // decides which monitor or window is captured, and guessing here is
    // exactly how the wrong screen gets published.
    if (pipewireNodeId < 0)
        return false;
    if (m_screenSharing)
        stopScreenShare();
    const QString cid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qCInfo(lcSfuCall) << "screen share publishing node=" << pipewireNodeId
                      << "encrypted=" << m_roomEncrypted;
    m_client->sfuAddTrack(cid, QStringLiteral("screen"), 1,
                          SfuMediaEngine::kScreenWidth,
                          SfuMediaEngine::kScreenHeight,
                          true, m_roomEncrypted);
    m_engine->publishVideo(cid, /*screenShare=*/true, pipewireNodeId,
                           pipewireFd);
    m_screenCid = cid;
    m_publishedTrackIds.append(cid);
    m_screenSharing = true;
    // A NEW share is a new identity for the stage. See m_localShareEpoch:
    // without this, stopping and restarting our own share would reuse one
    // share id, and a viewer who had dismissed the first from their
    // spotlight would silently never be offered the second.
    ++m_localShareEpoch;
    Q_EMIT mediaStateChanged();
    return true;
#else
    Q_UNUSED(pipewireNodeId); Q_UNUSED(pipewireFd);
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
    Q_EMIT mediaStateChanged();
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
#ifdef HAVE_LIGHTNING_WEBRTC
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
            return;
        // Only when the server's answer differs from ours. Reconciling
        // against the REPORTED state rather than remembering what we last
        // sent is what makes this idempotent: it converges from whatever the
        // server currently believes, including a mute it inferred itself,
        // and it cannot loop because the request changes the reported value.
        if (track.value(QStringLiteral("muted")).toBool() == m_micMuted)
            return;
        m_client->sfuMuteTrack(sid, m_micMuted);
        return;
    }
#endif
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
    m_handRaised = raised;
    // LOCAL ONLY, and honestly so: nothing here reaches the SFU, the
    // MatrixRTC membership or a to-device message, so no peer can see it.
    // The badge on OUR row is real feedback that the toggle took; a badge on
    // anyone else's could never light, and inventing a wire representation
    // for it is a protocol decision to be checked against a real
    // element-call client, not guessed at here.
    if (m_participantModel && !m_ownIdentity.isEmpty())
        m_participantModel->setHandRaised(m_ownIdentity, m_handRaised);
    Q_EMIT mediaStateChanged();
}

void SfuCallController::toggleHandRaised() { setHandRaised(!m_handRaised); }

void SfuCallController::setParticipantVolume(const QString &identity,
                                              int percent)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    // Local only: nothing is sent, and nobody else is affected.
    if (!m_engine.isNull())
        m_engine->setParticipantVolume(identity, percent);
#else
    Q_UNUSED(percent);
#endif
    // Recorded so the control that sets it can READ IT BACK. This was
    // write-only, which is why no QML ever called it: a slider with nothing
    // to bind to cannot show the value it just set.
    if (m_participantModel)
        m_participantModel->setVolumePercent(identity, percent);
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
                row.cameraOn = !muted;
            } else if (source == QLatin1String("screen_share")) {
                row.screenSharing = !muted;
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
            // surface, which gates on this flag, drew nothing at all. We
            // know what we asked for; OR it in. The mute state is likewise
            // ours: syncMicMuteToSfu() converges the server towards it, so
            // reading it back from the server was reading our own intent
            // through a delay.
            row.micKnown = true;
            row.micMuted = m_micMuted;
            row.cameraKnown = true;
            row.cameraOn = row.cameraOn || m_cameraOn;
            row.screenSharing = row.screenSharing || m_screenSharing;
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
