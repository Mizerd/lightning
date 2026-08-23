#include "calls/SfuCallController.h"

#include <QRandomGenerator>
#include <QUuid>
#include <QVariantMap>

#include "calls/RtcController.h"
#include "matrix/MatrixClient.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/CallFrameCryptor.h"
#include "calls/SfuMediaEngine.h"
#endif

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
    connect(m_client, &MatrixClient::loggedOut, this,
            [this] { teardown(State::Ended); });
}

void SfuCallController::setRtcController(RtcController *rtc)
{
    m_rtc = rtc;
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
        setState(State::Failed, tr("This build has no calling media support."));
        return false;
    }
    if (!m_rtc) {
        setState(State::Failed, tr("Calling isn't ready yet."));
        return false;
    }

    // THE safety gate. An encrypted room whose media cannot be encrypted is
    // refused outright rather than joined in the clear: the user was told
    // that room is end-to-end encrypted, and carrying their audio where the
    // SFU can read it would make that untrue.
    const QString block = m_rtc->joinBlockReason(roomId);
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

    ++m_generation;
    m_roomId = roomId;
    m_withVideo = withVideo;
    m_cameraOn = withVideo;
    m_screenSharing = false;
    m_handRaised = false;
    m_participants.clear();
    m_speaking.clear();
    m_publishedTrackIds.clear();
    m_membershipEventId.clear();
    m_delayId.clear();
    m_ownIdentity.clear();
    m_mediaEncrypted = false;

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
    if (m_client->sfuConnect(m_focusUrl, m_roomId) == 0) {
        teardown(State::Failed, tr("Couldn't connect to the call."));
        Q_EMIT callFailed(m_lastError);
    }
}

void SfuCallController::onSfuState(const QString &state,
                                    const QString &category)
{
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
    if (!active())
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    m_ownIdentity = identity;
    m_participants = participants.mid(0, kMaxParticipants);
    if (!m_engine.isNull()) {
        m_engine->start();
        m_engine->setIceServers(iceServers);
        applyAudioState();
        publishTracks();
    }
    setState(State::Connecting);
    Q_EMIT participantsChanged();
    // Everyone who is already here needs our key, and we need theirs.
    rotateAndDistributeKey();
#else
    Q_UNUSED(identity); Q_UNUSED(participants); Q_UNUSED(iceServers);
#endif
}

void SfuCallController::publishTracks()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (m_engine.isNull() || !m_client)
        return;
    // The client chooses the track id and DECLARES it before negotiating, so
    // the SFU can map the negotiated media section to the track it
    // authorized. Declaring and publishing must use the same id.
    const QString audioCid =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_client->sfuAddTrack(audioCid, QStringLiteral("microphone"), 0, false);
    m_engine->publishAudio(audioCid);
    m_publishedTrackIds.append(audioCid);

    if (m_cameraOn) {
        const QString videoCid =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_client->sfuAddTrack(videoCid, QStringLiteral("camera"), 1, false);
        m_engine->publishVideo(videoCid, /*screenShare=*/false,
                               /*nodeId=*/-1);
        m_publishedTrackIds.append(videoCid);
    }
#endif
}

void SfuCallController::onSfuParticipants(const QVariantList &participants)
{
    if (!active())
        return;
    const int before = m_participants.size();
    m_participants = participants.mid(0, kMaxParticipants);
    Q_EMIT participantsChanged();
    // A LEAVER must stop being able to decrypt, so any change in the set
    // rotates the key. Rotating on joins too is the simple, safe choice:
    // the alternative is tracking who is new, and being wrong about that
    // means someone keeps a key they should not have.
    if (m_participants.size() != before)
        rotateAndDistributeKey();
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

void SfuCallController::onEngineLocalDescription(int target,
                                                  const QString &kind,
                                                  const QString &sdp)
{
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
    if (!active() || !m_client)
        return;
    m_client->sfuLocalCandidate(
        target == 0 ? QStringLiteral("publisher")
                    : QStringLiteral("subscriber"),
        candidateInit);
}

void SfuCallController::onEngineFailed(const QString &category)
{
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
    Q_UNUSED(sender);
    Q_UNUSED(claimedDeviceId);
    if (!active() || roomId != m_roomId)
        return;
#ifdef HAVE_LIGHTNING_WEBRTC
    // Keys are held by the engine's cryptor, which is the only thing that
    // needs them. They are never stored, never logged, and never reach QML.
    Q_UNUSED(keyIndex);
    Q_UNUSED(keyBase64);
    // Wiring the received key into the per-participant cryptor requires the
    // pipeline-level frame routing that is not attached yet; until it is,
    // an encrypted room cannot be joined at all (see join()), so there is
    // nothing here that could silently proceed unencrypted.
#else
    Q_UNUSED(keyIndex); Q_UNUSED(keyBase64);
#endif
}

void SfuCallController::rotateAndDistributeKey()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!active() || !m_client)
        return;
    // Only meaningful once the frame cryptor is attached to the pipeline.
    // Until then this deliberately does nothing rather than distributing a
    // key nothing uses — a key on the wire implies media is encrypted.
    if (!m_mediaEncrypted)
        return;
    m_keyIndex = (m_keyIndex + 1) % 16;
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
    ++m_generation;
    m_refreshTimer.stop();
    m_publishOp = 0;

#ifdef HAVE_LIGHTNING_WEBRTC
    // Media FIRST: release the microphone and camera before anything that
    // can fail or block. No device stays live because a network call hung.
    if (!m_engine.isNull())
        m_engine->stop();
#endif
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
        m_client->sfuAddTrack(cid, QStringLiteral("camera"), 1, false);
        m_engine->publishVideo(cid, false, -1);
        m_publishedTrackIds.append(cid);
    } else {
        // Stop the LAST video track we published. The camera must actually
        // go off, not merely stop being rendered — the LED is the user's
        // only unambiguous indicator.
        for (int i = m_publishedTrackIds.size() - 1; i >= 0; --i) {
            m_engine->unpublish(m_publishedTrackIds.at(i));
            m_publishedTrackIds.removeAt(i);
            break;
        }
    }
    Q_EMIT mediaStateChanged();
#else
    Q_UNUSED(on);
#endif
}

void SfuCallController::toggleCamera() { setCameraOn(!m_cameraOn); }

bool SfuCallController::startScreenShare(int pipewireNodeId)
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
    m_client->sfuAddTrack(cid, QStringLiteral("screen"), 1, true);
    m_engine->publishVideo(cid, /*screenShare=*/true, pipewireNodeId);
    m_publishedTrackIds.append(cid);
    m_screenSharing = true;
    Q_EMIT mediaStateChanged();
    return true;
#else
    Q_UNUSED(pipewireNodeId);
    return false;
#endif
}

void SfuCallController::stopScreenShare()
{
#ifdef HAVE_LIGHTNING_WEBRTC
    if (!m_screenSharing || m_engine.isNull())
        return;
    for (int i = m_publishedTrackIds.size() - 1; i >= 0; --i) {
        m_engine->unpublish(m_publishedTrackIds.at(i));
        m_publishedTrackIds.removeAt(i);
        break;
    }
    m_screenSharing = false;
    Q_EMIT mediaStateChanged();
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
        // MatrixRTC identities are "{userId}:{deviceId}" for the session
        // format. Split on the LAST colon: a Matrix user id contains one.
        const int split = identity.lastIndexOf(QLatin1Char(':'));
        QVariantMap row;
        row.insert(QStringLiteral("identity"), identity);
        row.insert(QStringLiteral("userId"),
                   split > 0 ? identity.left(split) : identity);
        row.insert(QStringLiteral("deviceId"),
                   split > 0 ? identity.mid(split + 1) : QString());
        row.insert(QStringLiteral("local"), identity == m_ownIdentity);
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
        out.append(row);
    }
    return out;
}
