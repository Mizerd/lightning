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
                    if (pipewireFd >= 0)
                        ::close(pipewireFd);
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
    Q_EMIT participantsChanged();
    // A LEAVER must stop being able to decrypt, so any change in the set
    // rotates the key. Rotating on joins too is the simple, safe choice:
    // the alternative is tracking who is new, and being wrong about that
    // means someone keeps a key they should not have.
    QSet<QString> after;
    for (const QVariant &row : std::as_const(m_participants)) {
        after.insert(
            row.toMap().value(QStringLiteral("identity")).toString());
    }
    if (after != before)
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

void SfuCallController::onSfuSpeakers(const QVariantList &speakers)
{
    if (!active())
        return;
    m_speaking.clear();
    for (const QVariant &value : speakers) {
        const QVariantMap entry = value.toMap();
        const QString sid = entry.value(QStringLiteral("sid")).toString();
        if (!sid.isEmpty())
            m_speaking.insert(sid, entry.value(QStringLiteral("active")).toBool());
    }
    Q_EMIT participantsChanged();
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

void SfuCallController::detachVideoSink(const QString &identity)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    const QString cameraKey = trackKeyForSource(
        identity, QStringLiteral("camera"));
    if (!cameraKey.isEmpty())
        m_videoRouter->detachSink(cameraKey);
    const QString streamId = streamIdForIdentity(identity);
    if (!streamId.isEmpty())
        m_videoRouter->detachSink(streamId);
#else
    Q_UNUSED(identity);
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

void SfuCallController::detachScreenSink(const QString &identity)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    const QString key = trackKeyForSource(identity,
                                          QStringLiteral("screen_share"));
    if (key.isEmpty())
        return;
    m_videoRouter->detachSink(key);
#else
    Q_UNUSED(identity);
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

void SfuCallController::detachLocalCameraSink()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    m_videoRouter->detachSink(SfuMediaEngine::localCameraStreamId());
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

void SfuCallController::detachLocalScreenSink()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_videoRouter)
        return;
    m_videoRouter->detachSink(SfuMediaEngine::localScreenStreamId());
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
    m_publishedTrackIds.clear();
    m_audioCid.clear();
    m_cameraCid.clear();
    m_screenCid.clear();
    m_cameraOn = false;
    m_screenSharing = false;
    m_handRaised = false;
    m_mediaEncrypted = false;
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
    Q_EMIT mediaStateChanged();
}

void SfuCallController::toggleHandRaised() { setHandRaised(!m_handRaised); }

void SfuCallController::setParticipantVolume(const QString &identity,
                                              int percent)
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull())
        return;
    // Local only: nothing is sent, and nobody else is affected.
    m_engine->setParticipantVolume(identity, percent);
#else
    Q_UNUSED(identity); Q_UNUSED(percent);
#endif
}

QVariantList SfuCallController::participants() const
{
    QVariantList out;
    for (const QVariant &value : m_participants) {
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
        QVariantMap row;
        row.insert(QStringLiteral("identity"), identity);
        row.insert(QStringLiteral("userId"),
                   person.value(QStringLiteral("userId")).toString());
        // Room-resolved profile, so a tile draws a real name and avatar.
        // Empty means "not known here" and the tile falls back to initials
        // rather than inventing anything.
        row.insert(QStringLiteral("displayName"),
                   person.value(QStringLiteral("displayName")).toString());
        row.insert(QStringLiteral("avatarMxc"),
                   person.value(QStringLiteral("avatarMxc")).toString());
        // "local" is this DEVICE. The membership knows; identity equality is
        // kept as the fallback for a session whose membership has not landed
        // yet, so the local tile is never mislabelled as someone else.
        const bool ownDevice =
            person.value(QStringLiteral("ownDevice")).toBool();
        row.insert(QStringLiteral("local"),
                   ownDevice || identity == m_ownIdentity);
        row.insert(QStringLiteral("speaking"),
                   m_speaking.value(
                       entry.value(QStringLiteral("sid")).toString(), false));
        // Track state as the SFU reports it. Absent means UNKNOWN, and the
        // UI must render nothing rather than a confident "not muted".
        bool micKnown = false;
        bool micMuted = false;
        bool cameraKnown = false;
        bool cameraOn = false;
        bool sharing = false;
        for (const QVariant &t : entry.value(QStringLiteral("tracks")).toList()) {
            const QVariantMap track = t.toMap();
            const QString source =
                track.value(QStringLiteral("source")).toString();
            const bool muted = track.value(QStringLiteral("muted")).toBool();
            if (source == QLatin1String("microphone")) {
                micKnown = true;
                micMuted = muted;
            } else if (source == QLatin1String("camera")) {
                cameraKnown = true;
                cameraOn = !muted;
            } else if (source == QLatin1String("screen_share")) {
                sharing = !muted;
            }
        }
        row.insert(QStringLiteral("micKnown"), micKnown);
        row.insert(QStringLiteral("micMuted"), micMuted);
        row.insert(QStringLiteral("cameraKnown"), cameraKnown);
        row.insert(QStringLiteral("cameraOn"), cameraOn);
        row.insert(QStringLiteral("screenSharing"), sharing);
        // The routing keys, so a tile can RE-ATTACH when they change. The SFU
        // can announce a participant before it announces which section their
        // tracks landed on, and an attach that happened while the key was
        // still empty is a surface that never receives a frame. QML watches
        // these two values; it does not interpret them.
        row.insert(QStringLiteral("cameraTrackKey"),
                   trackKeyForSource(identity, QStringLiteral("camera")));
        row.insert(QStringLiteral("screenTrackKey"),
                   trackKeyForSource(identity,
                                     QStringLiteral("screen_share")));
        out.append(row);
    }
    return out;
}
