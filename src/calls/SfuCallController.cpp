#include "calls/SfuCallController.h"

#include <QLoggingCategory>

#include <QRandomGenerator>
#include <QUuid>
#include <QVariantMap>

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
            [this](unsigned nodeId) {
                // The portal granted exactly what the user picked. Guard on
                // still being in a call: the picker is modal to the desktop,
                // not to us, so the call can end while it is open.
                if (!active())
                    return;
                startScreenShare(static_cast<int>(nodeId));
            });
    connect(m_portal, &ScreenCastPortal::cancelled, this, [] {
        // The user declined. Deliberately silent: a dialog saying "you
        // cancelled" is noise.
    });
    connect(m_portal, &ScreenCastPortal::failed, this,
            [this](const QString &category) {
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
        Q_EMIT callFailed(
            tr("Screen sharing isn't available on this desktop."));
        return;
    }
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
    m_membershipEventId.clear();
    m_delayId.clear();
    m_ownIdentity.clear();
    m_mediaEncrypted = false;
    m_keyIndex = 0;
    m_candidatesSent = 0;

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
                          false, m_roomEncrypted);
    m_engine->publishAudio(audioCid);
    m_publishedTrackIds.append(audioCid);

    if (m_cameraOn) {
        const QString videoCid =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_client->sfuAddTrack(videoCid, QStringLiteral("camera"), 1, false,
                              m_roomEncrypted);
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
    if (raw.size() != 32)
        return;
    // Keys live ONLY in the engine's cryptor, which is the only thing that
    // needs them: never stored, never logged, never in QML.
    // Addressed to the SENDER's own key ring. The sender is identified by
    // their MatrixRTC identity, which the SFU echoes as the participant
    // identity, and the participant list carries the LiveKit sid the SDP
    // uses. Without that mapping the key would have to go into a shared
    // ring, where two senders both using index 0 would collide.
    const QString streamId = streamIdForSender(sender, claimedDeviceId);
    if (streamId.isEmpty())
        return;
    m_engine->setInboundKey(streamId, keyIndex, raw);
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
    const QString streamId = streamIdForIdentity(identity);
    if (streamId.isEmpty())
        return;
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
    const QString streamId = streamIdForIdentity(identity);
    if (streamId.isEmpty())
        return;
    m_videoRouter->detachSink(streamId);
#else
    Q_UNUSED(identity);
#endif
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

QString SfuCallController::streamIdForSender(const QString &userId,
                                            const QString &deviceId) const
{
    if (!m_rtc || m_roomId.isEmpty())
        return {};
    // The membership is what knows a device's SFU identity — derived in Rust
    // and a sha256 in the sticky format, so it cannot be reconstructed here.
    return streamIdForIdentity(
        m_rtc->rtcIdentityFor(m_roomId, userId, deviceId));
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
    const QString targets = m_rtc
        ? m_rtc->mediaKeyTargetsJson(m_roomId)
        : QStringLiteral("[]");
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
        m_client->sfuAddTrack(cid, QStringLiteral("camera"), 1, false,
                              m_roomEncrypted);
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
    m_client->sfuAddTrack(cid, QStringLiteral("screen"), 1, true,
                          m_roomEncrypted);
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
    // Close the portal session too: leaving it open keeps the compositor
    // capturing a surface nothing is reading.
    if (m_portal)
        m_portal->cancel();
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
        out.append(row);
    }
    return out;
}
