// The MSC2746 state machine in CallController, driven with synthetic
// CallSignal observations through a recording client double. The outbound
// path is exercised through placeCallWithOffer with a synthetic SDP.
#include <QtTest/QtTest>

#include <QDateTime>
#include <QMetaProperty>
#include <QSignalSpy>

#include <QAbstractItemModel>

#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/CallParticipantModel.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#include "calls/SdpStore.h"
#include "calls/RtcController.h"
#include <QSettings>
#include <QTemporaryDir>
#include "app/SettingsManager.h"
#include "calls/SfuCallController.h"
#include "matrix/CallSignal.h"
#include "matrix/RtcSession.h"
#include "matrix/MockMatrixClient.h"

namespace {

struct SentEvent {
    QString kind;
    QString roomId;
    QString callId;
    QString partyId;
    QString extra; // reason / selected party / notification event id
};

class RecordingCallClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsCallSignaling() const override { return true; }
    QString currentUserId() const override
    {
        return simulatedUserId.isEmpty() ? MockMatrixClient::currentUserId()
                                         : simulatedUserId;
    }
    QString simulatedUserId;

    quint64 callInvite(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &offerType,
                       const QString &offerSdp, quint64 lifetimeMs,
                       const QString &invitee) override
    {
        Q_UNUSED(offerType); Q_UNUSED(offerSdp); Q_UNUSED(lifetimeMs);
        Q_UNUSED(invitee);
        sent.append({QStringLiteral("invite"), roomId, callId, partyId, {}});
        return ++opCounter;
    }
    quint64 callReject(const QString &roomId, const QString &callId,
                       const QString &partyId) override
    {
        sent.append({QStringLiteral("reject"), roomId, callId, partyId, {}});
        return ++opCounter;
    }
    quint64 callHangup(const QString &roomId, const QString &callId,
                       const QString &partyId,
                       const QString &reason) override
    {
        sent.append({QStringLiteral("hangup"), roomId, callId, partyId,
                     reason});
        return ++opCounter;
    }
    quint64 callSelectAnswer(const QString &roomId, const QString &callId,
                             const QString &partyId,
                             const QString &selectedPartyId) override
    {
        sent.append({QStringLiteral("select_answer"), roomId, callId,
                     partyId, selectedPartyId});
        return ++opCounter;
    }
    quint64 callRtcDecline(const QString &roomId,
                           const QString &notificationEventId) override
    {
        sent.append({QStringLiteral("rtc_decline"), roomId, {}, {},
                     notificationEventId});
        return ++opCounter;
    }

    quint64 callAnswer(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &answerType,
                       const QString &answerSdp) override
    {
        Q_UNUSED(answerType);
        // Record that an answer was dispatched without retaining the SDP,
        // mirroring production's no-echo rule.
        sent.append({QStringLiteral("answer"), roomId, callId, partyId,
                     answerSdp.isEmpty() ? QString()
                                         : QStringLiteral("<sdp>")});
        return ++opCounter;
    }
    void setCallMediaCapable(bool capable) override
    {
        mediaCapable = capable;
    }
    quint64 callCandidates(const QString &roomId, const QString &callId,
                           const QString &partyId,
                           const QVariantList &candidates) override
    {
        Q_UNUSED(roomId); Q_UNUSED(partyId);
        candidateBatches.append(qMakePair(callId, candidates));
        return ++opCounter;
    }
    quint64 requestCallTurnServers() override
    {
        lastTurnOp = ++opCounter;
        return lastTurnOp;
    }
    // The SFU's only removal-shaped verb: there is no unpublish message, so a
    // stopped camera or share must arrive here.
    void sfuMuteTrack(const QString &sid, bool muted) override
    {
        muteRequests.append(qMakePair(sid, muted));
    }
    QList<QPair<QString, bool>> muteRequests;

    // MatrixRTC membership: everything the leave path depends on.
    quint64 rtcPublishMembership(const QString &roomId,
                                 const QString &focusUrl,
                                 const QString &intent) override
    {
        Q_UNUSED(focusUrl); Q_UNUSED(intent);
        publishes.append(roomId);
        lastPublishOp = ++opCounter;
        return lastPublishOp;
    }
    quint64 rtcRestartDelayedLeave(const QString &delayId) override
    {
        delayedRestarts.append(delayId);
        lastRestartOp = ++opCounter;
        return lastRestartOp;
    }
    // MatrixRTC session reads. Names and avatars come from the membership,
    // which arrives on a different transport from the SFU's participant
    // list, so a test can deliver them in either order.
    bool supportsMatrixRtc() const override { return true; }
    quint64 rtcSession(const QString &roomId, bool preferServer) override
    {
        sessionReads.append(roomId);
        sessionReadPreferredServer.append(preferServer);
        lastSessionOp = ++opCounter;
        return lastSessionOp;
    }
    void answerSession(quint64 opId, const RtcSessionData &session)
    {
        Q_EMIT rtcSessionReceived(opId, session);
    }
    QStringList sessionReads;
    QList<bool> sessionReadPreferredServer;
    quint64 lastSessionOp = 0;

    quint64 rtcRetractMembership(const QString &roomId,
                                 const QString &delayId) override
    {
        retractions.append(qMakePair(roomId, delayId));
        lastRetractOp = ++opCounter;
        return lastRetractOp;
    }
    QStringList publishes;
    QStringList delayedRestarts;
    QList<QPair<QString, QString>> retractions;
    quint64 lastPublishOp = 0;
    quint64 lastRestartOp = 0;
    quint64 lastRetractOp = 0;
    /// The bridge routes `rtc_membership_retracted` and `rtc_delayed_updated`
    /// onto this one signal, distinguished only by op id; emitted exactly as
    /// RustSdkMatrixClient does.
    void answerMembershipOp(quint64 opId, bool ok, const QString &category)
    {
        Q_EMIT rtcMembershipRetracted(opId, ok, category);
    }
    void answerPublish(quint64 opId, bool ok, const QString &delayId,
                       const QString &delayedCategory = QString())
    {
        Q_EMIT rtcMembershipPublished(opId, ok, QString(),
                                      QStringLiteral("$event"), delayId,
                                      delayedCategory);
    }
    /// A publish the homeserver refused, with the category the bridge really
    /// sends (`answerPublish` hard-codes an empty one).
    void refusePublish(quint64 opId, const QString &category)
    {
        Q_EMIT rtcMembershipPublished(opId, false, category, QString(),
                                      QString(), QString());
    }
    /// The SFU's own lifecycle, on the signal SfuCallController connects.
    void emitSfuState(const QString &state, const QString &category)
    {
        Q_EMIT sfuStateChanged(state, category);
    }
    /// A raise or lower off the sync loop. A raise names the membership event
    /// it annotates; a lower names only the reaction, as a redaction does.
    void emitHandChanged(const QString &roomId, const QString &sender,
                         const QString &membershipEventId,
                         const QString &reactionEventId, bool raised)
    {
        Q_EMIT rtcHandChanged(roomId, sender, membershipEventId,
                              reactionEventId, raised);
    }
    /// A transient call reaction, on the bridge's real signature. Records the
    /// wire arguments: referenced membership event and (emoji, name) pair.
    struct SentReaction {
        QString roomId;
        QString membershipEventId;
        QString emoji;
        QString name;
    };
    quint64 rtcSendCallReaction(const QString &roomId,
                                const QString &membershipEventId,
                                const QString &emoji,
                                const QString &name) override
    {
        reactionSends.append({roomId, membershipEventId, emoji, name});
        lastReactionOp = ++opCounter;
        return lastReactionOp;
    }
    QList<SentReaction> reactionSends;
    quint64 lastReactionOp = 0;
    /// One arriving off the sync loop.
    void emitCallReaction(const QString &roomId, const QString &sender,
                          const QString &membershipEventId,
                          const QString &emoji)
    {
        Q_EMIT rtcCallReactionReceived(roomId, sender, membershipEventId,
                                       emoji);
    }
    /// The generic RTC send answer, which a reaction's result rides.
    void answerRtcSend(quint64 opId, bool ok, const QString &category)
    {
        Q_EMIT rtcSendFinished(opId, ok, category, QString());
    }
    /// Answered with a real op id so a successful membership publish reaches
    /// Authorizing.
    quint64 sfuConnect(const QString &serviceUrl,
                       const QString &roomId) override
    {
        sfuConnects.append(qMakePair(serviceUrl, roomId));
        return ++opCounter;
    }
    QList<QPair<QString, QString>> sfuConnects;
    void emitCandidates(const QString &roomId, const QString &callId,
                        bool own, const QVariantList &candidates)
    {
        Q_EMIT callCandidatesReceived(roomId, callId,
                                      QStringLiteral("peer-party"), own,
                                      candidates);
    }
    void emitTurnServers(quint64 opId, bool ok, const QStringList &uris)
    {
        Q_EMIT callTurnServersReceived(opId, ok, QStringLiteral("u"),
                                       QStringLiteral("p"), uris, 600,
                                       QString());
    }
    QList<QPair<QString, QVariantList>> candidateBatches;
    quint64 lastTurnOp = 0;
    QString takeCallSessionDescription(const QString &eventId) override
    {
        takenDescriptions.append(eventId);
        return storedDescriptions.take(eventId);
    }
    bool mediaCapable = false;
    QHash<QString, QString> storedDescriptions;
    QStringList takenDescriptions;

    void emitSignal(const CallSignal &signal)
    {
        Q_EMIT callSignalReceived(signal);
    }
    void emitLoggedOut() { Q_EMIT loggedOut(); }
    void emitSendFinished(quint64 opId, bool ok, const QString &category)
    {
        Q_EMIT callSendFinished(opId, ok, category, QString(), QString());
    }

    QList<SentEvent> sent;
    quint64 opCounter = 100;
};

// State-only media double (FakeRecorder pattern): records the calls the
// controller makes and lets the test drive the async results by hand.
class FakeMediaBackend : public CallMediaBackend
{
public:
    using CallMediaBackend::CallMediaBackend;

    void createOffer(const QString &callId) override
    {
        offerRequests.append(callId);
    }
    void createAnswer(const QString &callId,
                      const QString &remoteOfferSdp) override
    {
        answerRequests.append(callId);
        lastRemoteOffer = remoteOfferSdp;
    }
    void setRemoteAnswer(const QString &callId,
                         const QString &remoteAnswerSdp) override
    {
        remoteAnswers.append(callId);
        lastRemoteAnswer = remoteAnswerSdp;
    }
    void addRemoteCandidate(const QString &callId, const QString &candidate,
                            const QString &sdpMid, int sdpMLineIndex) override
    {
        Q_UNUSED(sdpMid); Q_UNUSED(sdpMLineIndex);
        remoteCandidates.append(callId + QLatin1Char('|') + candidate);
    }
    void setIceServers(const QStringList &uris, const QString &username,
                       const QString &password) override
    {
        Q_UNUSED(username); Q_UNUSED(password);
        iceServerApplications.append(uris);
    }
    void close(const QString &callId) override { closed.append(callId); }

    void deliverOffer(const QString &callId, const QString &sdp)
    {
        Q_EMIT offerReady(callId, sdp);
    }
    void deliverAnswer(const QString &callId, const QString &sdp)
    {
        Q_EMIT answerReady(callId, sdp);
    }
    void deliverConnected(const QString &callId)
    {
        Q_EMIT connected(callId);
    }
    void deliverFailure(const QString &callId, const QString &category)
    {
        Q_EMIT failed(callId, category);
    }
    void deliverLocalCandidate(const QString &callId, const QString &line)
    {
        Q_EMIT localCandidate(callId, line, QStringLiteral("0"), 0);
    }
    void deliverGatheringComplete(const QString &callId)
    {
        Q_EMIT gatheringComplete(callId);
    }

    QStringList remoteCandidates;
    QList<QStringList> iceServerApplications;
    QStringList offerRequests;
    QStringList answerRequests;
    QStringList remoteAnswers;
    QStringList closed;
    QString lastRemoteOffer;
    QString lastRemoteAnswer;
};

/// FakeMediaBackend plus real mute bookkeeping. Separate because the base
/// double keeps `supportsMuteControl()` false for the "no mute without an
/// engine that implements it" case.
class MuteTrackingBackend : public FakeMediaBackend
{
public:
    using FakeMediaBackend::FakeMediaBackend;

    void setMicrophoneMuted(const QString &callId, bool muted) override
    {
        micCalls.append(callId);
        micMuted = muted;
    }
    void setOutputMuted(const QString &callId, bool muted) override
    {
        outputCalls.append(callId);
        outputMuted = muted;
    }
    bool supportsMuteControl() const override { return true; }

    QStringList micCalls;
    QStringList outputCalls;
    bool micMuted = false;
    bool outputMuted = false;
};


CallSignal freshInvite(const QString &callId,
                       const QString &roomId = QStringLiteral("!r:x"),
                       const QString &sender = QStringLiteral("@peer:x"))
{
    CallSignal s;
    s.kind = CallSignal::Kind::Invite;
    s.roomId = roomId;
    s.eventId = QStringLiteral("$invite-") + callId;
    s.sender = sender;
    s.callId = callId;
    s.partyId = QStringLiteral("peer-party");
    s.lifetimeMs = 60000;
    s.originServerTs = QDateTime::currentMSecsSinceEpoch();
    s.version = QStringLiteral("1");
    s.sessionType = QStringLiteral("offer");
    s.hasDescription = true;
    return s;
}

// Call stage data layer: these drive SfuCallController through the private
// merge helpers the SFU slots use, via test seams.

QVariantMap sfuTrack(const QString &source, const QString &sid, bool muted)
{
    QVariantMap track;
    track.insert(QStringLiteral("source"), source);
    track.insert(QStringLiteral("sid"), sid);
    track.insert(QStringLiteral("muted"), muted);
    return track;
}

QVariantMap sfuParticipant(const QString &identity, const QString &sid,
                           const QVariantList &tracks)
{
    QVariantMap row;
    row.insert(QStringLiteral("identity"), identity);
    row.insert(QStringLiteral("sid"), sid);
    row.insert(QStringLiteral("tracks"), tracks);
    return row;
}

QVariantMap speakerEntry(const QString &sid, bool active)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("sid"), sid);
    entry.insert(QStringLiteral("active"), active);
    return entry;
}

QVariantMap speakerEntry(const QString &sid, bool active, double level)
{
    QVariantMap entry = speakerEntry(sid, active);
    entry.insert(QStringLiteral("level"), level);
    return entry;
}

QVariant participantRole(const CallParticipantModel *model, int row,
                         CallParticipantModel::Roles role)
{
    return model->data(model->index(row, 0), role);
}

int participantRowFor(const CallParticipantModel *model,
                      const QString &identity)
{
    return model->indexOfIdentity(identity);
}

/// Reaction emoji by their UTF-8 bytes. `QStringLiteral` would turn `\xF0`
/// into the UTF-16 unit U+00F0, silently testing the wrong string;
/// `QString::fromUtf8` gives the bytes that go on the wire.
const QString kThumbsUpEmoji = QString::fromUtf8("\xF0\x9F\x91\x8D");
const QString kPartyEmoji = QString::fromUtf8("\xF0\x9F\x8E\x89");

/// A call fixture with one remote participant whose membership is known, so
/// a reaction can be attributed. Returns the model.
CallParticipantModel *stageOneRemoteParticipant(
    RecordingCallClient &client, RtcController &rtc, SfuCallController &call,
    const QString &room, const QString &identity, const QString &userId,
    const QString &deviceId, const QString &membership)
{
    rtc.setClient(&client);
    rtc.setPokeCoalesceMsForTest(0);
    call.setClient(&client);
    call.setRtcController(&rtc);
    call.setMembershipForTest(room, QString());
    call.setCallStateForTest(SfuCallController::State::Connected);
    call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));
    call.ingestParticipantsForTest({
        sfuParticipant(identity, QStringLiteral("PA_ONE"), {}),
    });

    RtcParticipant member;
    member.userId = userId;
    member.deviceId = deviceId;
    member.rtcIdentity = identity;
    member.intent = QStringLiteral("audio");
    member.membershipEventId = membership;
    member.wireFormat = QStringLiteral("session");
    RtcSessionData session;
    session.roomId = room;
    session.participants = { member };
    rtc.refresh(room);
    client.answerSession(client.lastSessionOp, session);
    return call.participantModel();
}

} // namespace

class CallControllerTest : public QObject
{
    Q_OBJECT

private:
    // Helper, NOT a slot: QtTest treats every private slot as a test case.
    static void ringForRtcNotification(RecordingCallClient &client,
                                       const QString &roomId,
                                       const QString &sender)
    {
        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = roomId;
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = sender;
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        notify.callIntent = QStringLiteral("audio");
        client.emitSignal(notify);
    }

private Q_SLOTS:
    // A media key that arrives before join() must survive the join: the peer
    // sends its key as soon as it sees our membership, and nothing re-sends.
    // Not covered here: join() clearing the list before applyParkedKeys().
    // join() returns early without HAVE_LIGHTNING_WEBRTC, which this target
    // lacks, so that path is unreachable.
    void aKeyThatArrivesBeforeTheCallIsParkedRatherThanDiscarded()
    {
        SfuCallController call;
        const QString key = QString::fromUtf8(QByteArray(32, 'k').toBase64());
        // Idle, no room yet.
        QMetaObject::invokeMethod(
            &call, "onMediaKeyReceived", Qt::DirectConnection,
            Q_ARG(QString, QString()), Q_ARG(QString, QStringLiteral("@a:x")),
            Q_ARG(QString, QStringLiteral("DEV1")), Q_ARG(int, 0),
            Q_ARG(QString, key));
        QCOMPARE(call.parkedKeyCountForTest(), 1);

        // It survives a state change; only teardown and the TTL remove it.
        call.setCallStateForTest(SfuCallController::State::Connected);
        QCOMPARE(call.parkedKeyCountForTest(), 1);
    }

    // A malformed key never takes a slot, so a member cannot evict a
    // legitimate peer's key with rubbish.
    void aMalformedKeyIsRefusedBeforeItCanBeParked()
    {
        SfuCallController call;
        const auto park = [&call](const QString &device, int index,
                                  const QString &b64) {
            QMetaObject::invokeMethod(
                &call, "onMediaKeyReceived", Qt::DirectConnection,
                Q_ARG(QString, QString()),
                Q_ARG(QString, QStringLiteral("@a:x")),
                Q_ARG(QString, device), Q_ARG(int, index), Q_ARG(QString, b64));
        };
        const QString good = QString::fromUtf8(QByteArray(32, 'k').toBase64());
        park(QStringLiteral("DEV1"), 0, good);
        QCOMPARE(call.parkedKeyCountForTest(), 1);

        // Out-of-range index, oversized payload and a bad key length consume
        // no slot. The ring is 256 indices (element-call's), so out of range
        // is negative or >= 256.
        park(QStringLiteral("DEV2"), 256, good);
        park(QStringLiteral("DEV5"), -1, good);
        park(QStringLiteral("DEV6"), 100000, good);
        park(QStringLiteral("DEV3"), 0, QString(300, QLatin1Char('A')));
        park(QStringLiteral("DEV4"), 0,
             QString::fromUtf8(QByteArray(7, 'k').toBase64()));
        QCOMPARE(call.parkedKeyCountForTest(), 1);
    }

    // Key indices 16..255 are valid: matrix-js-sdk rotates key ids modulo
    // 256. Driven through the parking path, which this target compiles and
    // which is replayed through the same bound on join.
    void aMediaKeyAtAHighIndexIsKeptRatherThanDiscarded()
    {
        SfuCallController call;
        const QString key = QString::fromUtf8(QByteArray(16, 'k').toBase64());
        // One device per index: parking caps each device at two
        // (kMaxParkedKeysPerDevice).
        for (const int index : {16, 200, 255}) {
            QMetaObject::invokeMethod(
                &call, "onMediaKeyReceived", Qt::DirectConnection,
                Q_ARG(QString, QString()),
                Q_ARG(QString, QStringLiteral("@element:x")),
                Q_ARG(QString, QStringLiteral("ECDEV%1").arg(index)),
                Q_ARG(int, index), Q_ARG(QString, key));
        }
        QVERIFY2(call.parkedKeyCountForTest() == 3,
                 qPrintable(QStringLiteral("parked=%1: a key at index 16, 200 "
                                           "or 255 was discarded")
                                .arg(call.parkedKeyCountForTest())));
        // 256 is still out of the ring.
        QMetaObject::invokeMethod(
            &call, "onMediaKeyReceived", Qt::DirectConnection,
            Q_ARG(QString, QString()),
            Q_ARG(QString, QStringLiteral("@element:x")),
            Q_ARG(QString, QStringLiteral("ECDEV2")), Q_ARG(int, 256),
            Q_ARG(QString, key));
        QCOMPARE(call.parkedKeyCountForTest(), 3);
    }

    // A stale membership of this device (left by a session killed mid-call)
    // must not suppress the call announcement; another of our devices, or
    // anyone else, is a real participant.
    void aStaleOwnDeviceMembershipDoesNotSuppressTheAnnouncement()
    {
        const auto row = [](bool ownUser, bool ownDevice) {
            QVariantMap m;
            m.insert(QStringLiteral("userId"),
                     ownUser ? QStringLiteral("@me:x") : QStringLiteral("@b:x"));
            m.insert(QStringLiteral("ownUser"), ownUser);
            m.insert(QStringLiteral("ownDevice"), ownDevice);
            return QVariant(m);
        };
        using C = SfuCallController;
        QVERIFY(C::startsCallForAnnouncement({}));
        QVERIFY2(C::startsCallForAnnouncement({row(true, true)}),
                 "our own stale device membership suppressed the announcement");
        QVERIFY(!C::startsCallForAnnouncement({row(true, false)}));
        QVERIFY(!C::startsCallForAnnouncement({row(false, false)}));
        QVERIFY(!C::startsCallForAnnouncement({row(true, true),
                                               row(false, false)}));
    }

    // One slot per (sender, device, index): re-sending cannot grow the list,
    // and no sender can crowd out another.
    void oneSenderCannotCrowdOutAnothersParkedKey()
    {
        SfuCallController call;
        const QString key = QString::fromUtf8(QByteArray(32, 'k').toBase64());
        const auto park = [&call, &key](const QString &sender, int index) {
            QMetaObject::invokeMethod(
                &call, "onMediaKeyReceived", Qt::DirectConnection,
                Q_ARG(QString, QString()), Q_ARG(QString, sender),
                Q_ARG(QString, QStringLiteral("DEV")), Q_ARG(int, index),
                Q_ARG(QString, key));
        };
        park(QStringLiteral("@victim:x"), 0);
        QCOMPARE(call.parkedKeyCountForTest(), 1);
        // A flood from one sender: repeats replace and the device is capped.
        for (int i = 0; i < 40; ++i)
            park(QStringLiteral("@flood:x"), i % 16);
        QVERIFY2(call.parkedKeyCountForTest() <= 8,
                 qPrintable(QStringLiteral("parked=%1")
                                .arg(call.parkedKeyCountForTest())));
        // The victim's key is still there.
        QVERIFY(call.hasParkedKeyForTest(QStringLiteral("@victim:x")));
    }

    // Isolate QSettings so this suite never writes the user's config, and so
    // no value survives between runs to mask a failure.
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("call-controller-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    void inboundInviteRingsAndLocalRejectSendsOurParty()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);

        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        QSignalSpy ended(&calls, &CallController::incomingCallEnded);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));

        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(started.count(), 1);
        QCOMPARE(calls.activeCallId(), QStringLiteral("call-1"));

        QVERIFY(calls.rejectIncoming());
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::LocalReject);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("reject"));
        QCOMPARE(client.sent.first().callId, QStringLiteral("call-1"));
        QVERIFY(!client.sent.first().partyId.isEmpty());
    }

    void expiredInviteNeverRingsAndSendsNothing()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);

        CallSignal stale = freshInvite(QStringLiteral("old-call"));
        stale.originServerTs =
            QDateTime::currentMSecsSinceEpoch() - 120000; // 2 min ago
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        client.emitSignal(stale);

        QCOMPARE(calls.state(), CallController::State::Idle);
        QCOMPARE(started.count(), 0);
        QVERIFY(client.sent.isEmpty());
    }

    void glareSmallerCallIdSurvivesBothDirections()
    {
        // Ours is "zzz": theirs ("aaa") wins; we hang ours up with "replaced"
        // and ring theirs.
        {
            RecordingCallClient client;
            CallController calls;
            calls.setClient(&client);
            QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                             QStringLiteral("v=0 sdp")));
            // Force a known ordering relative to the generated id.
            const QString ours = calls.activeCallId();
            CallSignal theirs = freshInvite(QString());
            theirs.callId = QStringLiteral("0000-smaller"); // always < uuid
            client.emitSignal(theirs);

            QCOMPARE(calls.state(), CallController::State::Ringing);
            QCOMPARE(calls.activeCallId(), theirs.callId);
            bool hungUpOurs = false;
            for (const auto &event : client.sent) {
                if (event.kind == QLatin1String("hangup")
                    && event.callId == ours
                    && event.extra == QLatin1String("replaced"))
                    hungUpOurs = true;
            }
            QVERIFY(hungUpOurs);
        }
        // Ours is smaller: theirs is rejected, ours stays Inviting.
        {
            RecordingCallClient client;
            CallController calls;
            calls.setClient(&client);
            QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                             QStringLiteral("v=0 sdp")));
            const QString ours = calls.activeCallId();
            CallSignal theirs = freshInvite(QString());
            theirs.callId = QStringLiteral("zzzz-larger"); // > uuid hex
            client.emitSignal(theirs);

            QCOMPARE(calls.state(), CallController::State::Inviting);
            QCOMPARE(calls.activeCallId(), ours);
            bool rejectedTheirs = false;
            for (const auto &event : client.sent) {
                if (event.kind == QLatin1String("reject")
                    && event.callId == theirs.callId)
                    rejectedTheirs = true;
            }
            QVERIFY(rejectedTheirs);
        }
    }

    void secondInviteWhileRingingIsRejectedSessionUntouched()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        client.emitSignal(freshInvite(QStringLiteral("call-2"),
                                      QStringLiteral("!other:x")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeCallId(), QStringLiteral("call-1"));
        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("reject"));
        QCOMPARE(client.sent.first().callId, QStringLiteral("call-2"));
    }

    void ownAnswerFromAnotherDeviceEndsRingingSilently()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        CallSignal answer;
        answer.kind = CallSignal::Kind::Answer;
        answer.roomId = QStringLiteral("!r:x");
        answer.eventId = QStringLiteral("$answer-1");
        answer.sender = QStringLiteral("@me:x");
        answer.own = true;
        answer.callId = QStringLiteral("call-1");
        answer.partyId = QStringLiteral("my-other-device-party");
        client.emitSignal(answer);

        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(),
                 CallController::EndReason::AnsweredElsewhere);
        QVERIFY(client.sent.isEmpty());
    }

    void lateHangupForRetiredCallIsAbsorbed()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QVERIFY(calls.rejectIncoming());
        QCOMPARE(calls.state(), CallController::State::Ended);

        QSignalSpy stateSpy(&calls, &CallController::stateChanged);
        QSignalSpy endedSpy(&calls, &CallController::incomingCallEnded);
        CallSignal hangup;
        hangup.kind = CallSignal::Kind::Hangup;
        hangup.roomId = QStringLiteral("!r:x");
        hangup.eventId = QStringLiteral("$hangup-1");
        hangup.sender = QStringLiteral("@peer:x");
        hangup.callId = QStringLiteral("call-1");
        hangup.partyId = QStringLiteral("peer-party");
        hangup.reason = QStringLiteral("user_hangup");
        client.emitSignal(hangup);

        QCOMPARE(stateSpy.count(), 0);
        QCOMPARE(endedSpy.count(), 0);
    }

    void placeCallRefusesWithoutMediaBackend()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QVERIFY(!calls.placeCall(QStringLiteral("!r:x")));
        QCOMPARE(calls.lastRefusal(), QStringLiteral("no_media_backend"));
        QCOMPARE(calls.state(), CallController::State::Idle);
        QVERIFY(client.sent.isEmpty());
    }

    void outboundAnswerLocksPartyAndSendsSelectAnswerOnce()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                         QStringLiteral("v=0 sdp")));
        const QString callId = calls.activeCallId();

        CallSignal answer;
        answer.kind = CallSignal::Kind::Answer;
        answer.roomId = QStringLiteral("!r:x");
        answer.eventId = QStringLiteral("$answer-1");
        answer.sender = QStringLiteral("@peer:x");
        answer.callId = callId;
        answer.partyId = QStringLiteral("first-party");
        client.emitSignal(answer);
        QCOMPARE(calls.state(), CallController::State::Connecting);

        // A second answer from the peer's other device is ignored.
        CallSignal second = answer;
        second.eventId = QStringLiteral("$answer-2");
        second.partyId = QStringLiteral("second-party");
        client.emitSignal(second);

        int selectAnswers = 0;
        for (const auto &event : client.sent) {
            if (event.kind == QLatin1String("select_answer")) {
                ++selectAnswers;
                QCOMPARE(event.extra, QStringLiteral("first-party"));
            }
        }
        QCOMPARE(selectAnswers, 1);
        QCOMPARE(calls.state(), CallController::State::Connecting);
    }

    void mutedRoomStillRingsStateButShouldRingIsFalse()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setBacklogSuppressed(false);
        calls.setRoomMutedCheck(
            [](const QString &) { return true; });
        client.emitSignal(freshInvite(QStringLiteral("call-1")));

        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(!calls.shouldRing());
    }

    void backlogSuppressionDefaultsClosed()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        // Never wired a lifecycle owner: must not claim ring-worthiness.
        QVERIFY(!calls.shouldRing());
        calls.setBacklogSuppressed(false);
        QVERIFY(calls.shouldRing());
    }

    void targetedInviteForAnotherKnownUserIsDropped()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        invite.invitee = QStringLiteral("@someone-else:x");
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Idle);
        QVERIFY(client.sent.isEmpty());
    }

    void rtcNotificationRingsAndDeclineUsesRtcLane()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);

        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = QStringLiteral("!r:x");
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = QStringLiteral("@peer:x");
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        notify.callIntent = QStringLiteral("audio");
        client.emitSignal(notify);

        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(calls.rejectIncoming());
        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("rtc_decline"));
        QCOMPARE(client.sent.first().extra, QStringLiteral("$notify-1"));
    }

    void aDualStackCallerDoesNotGetAFalseDecline()
    {
        // One caller may announce a call on both lanes: an m.rtc.notification
        // and a legacy m.call.invite, whose ids never match. The invite must
        // not hit the busy branch and send m.call.reject while we ring.
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);

        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = QStringLiteral("!r:x");
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = QStringLiteral("@peer:x");
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        client.emitSignal(notify);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        // The same caller's legacy leg for the same conversation.
        CallSignal legacy = freshInvite(QStringLiteral("legacy-1"));
        legacy.roomId = QStringLiteral("!r:x");
        legacy.sender = QStringLiteral("@peer:x");
        client.emitSignal(legacy);

        // Still one ring, and nothing on the wire.
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeCallId(), QStringLiteral("$notify-1"));
        QVERIFY2(client.sent.isEmpty(),
                 "a second lane for the same call must not be rejected");
    }

    void aDifferentSenderInTheSameRoomIsStillRejectedAsBusy()
    {
        // The guard is narrow: varying only the sender proves the sender
        // clause matters.
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        CallSignal other = freshInvite(QStringLiteral("other-1"));
        other.roomId = QStringLiteral("!r:x"); // same room
        other.sender = QStringLiteral("@stranger:x"); // different person
        client.emitSignal(other);

        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("reject"));
    }

    void theSameSenderInADifferentRoomIsStillRejectedAsBusy()
    {
        // Varying only the room proves the room clause matters: the same
        // person in another room is a different call.
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        CallSignal other = freshInvite(QStringLiteral("other-1"));
        other.roomId = QStringLiteral("!other:x"); // different room
        other.sender = QStringLiteral("@peer:x"); // same person
        client.emitSignal(other);

        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("reject"));
    }

    void muteStopsPublishingAndDeafenRestoresThePriorMicState()
    {
        // Mute must reach the engine, and deafen must not unmute a microphone
        // muted before deafening.
        RecordingCallClient client;
        CallController calls;
        MuteTrackingBackend backend;
        calls.setClient(&client);
        calls.setMediaBackend(&backend);
        QVERIFY(calls.muteControlAvailable());

        // Reach a live call so the intent has somewhere to land.
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        const QString callId = calls.activeCallId();
        backend.deliverOffer(callId, QStringLiteral("v=0 offer"));
        CallSignal answer;
        answer.kind = CallSignal::Kind::Answer;
        answer.roomId = QStringLiteral("!r:x");
        answer.callId = callId;
        answer.partyId = QStringLiteral("peer");
        answer.sender = QStringLiteral("@peer:x");
        answer.hasDescription = true;
        client.emitSignal(answer);
        backend.deliverConnected(callId);
        QCOMPARE(calls.state(), CallController::State::Active);

        calls.setMicrophoneMuted(true);
        QVERIFY(calls.microphoneMuted());
        QCOMPARE(backend.micMuted, true);

        // Deafen while muted, then undeafen: the microphone stays muted.
        calls.setDeafened(true);
        QVERIFY(calls.deafened());
        QCOMPARE(backend.outputMuted, true);
        QCOMPARE(backend.micMuted, true);

        calls.setDeafened(false);
        QVERIFY(!calls.deafened());
        QCOMPARE(backend.outputMuted, false);
        QVERIFY2(calls.microphoneMuted(),
                 "undeafening must not unmute a mic the user muted first");
        QCOMPARE(backend.micMuted, true);
    }

    void aStandingMuteIsAppliedBeforeMediaCanFlow()
    {
        // Mute/deafen intent must reach the engine before media connects;
        // onMediaConnected arrives queued, after RTP is already flowing. So
        // the assertion is made before deliverConnected().
        RecordingCallClient client;
        CallController calls;
        MuteTrackingBackend backend;
        calls.setClient(&client);
        calls.setMediaBackend(&backend);

        calls.setMicrophoneMuted(true);
        calls.setDeafened(true);
        backend.micCalls.clear();
        backend.outputCalls.clear();
        backend.micMuted = false;    // engine state resets per session
        backend.outputMuted = false;

        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        QVERIFY2(backend.micMuted,
                 "the mic must be muted before any media can flow");
        QVERIFY2(backend.outputMuted,
                 "deafen must apply before any remote track can be heard");
    }

    void aStandingMuteIsAppliedBeforeAnsweringToo()
    {
        RecordingCallClient client;
        CallController calls;
        MuteTrackingBackend backend;
        calls.setClient(&client);
        calls.setMediaBackend(&backend);
        client.mediaCapable = true;

        CallSignal invite = freshInvite(QStringLiteral("inbound-1"));
        client.storedDescriptions.insert(invite.eventId,
                                         QStringLiteral("v=0 remote offer"));
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        calls.setMicrophoneMuted(true);
        backend.micMuted = false; // as a fresh pipeline would start

        QVERIFY(calls.answer());
        QVERIFY2(backend.micMuted,
                 "answering must not publish a muted user live");
    }

    void signingOutClearsTheAudioIntent()
    {
        // Mute/deafen persists between calls, but an account change clears it.
        RecordingCallClient client;
        CallController calls;
        MuteTrackingBackend backend;
        calls.setClient(&client);
        calls.setMediaBackend(&backend);
        calls.setDeafened(true);
        QVERIFY(calls.deafened());
        QVERIFY(calls.microphoneMuted());

        client.emitLoggedOut();
        QVERIFY(!calls.deafened());
        QVERIFY(!calls.microphoneMuted());
    }

    void muteControlIsUnavailableWithoutAnEngineThatImplementsIt()
    {
        // The seam's default is a no-op, so an engine that does not override
        // it must not show a working-looking control.
        RecordingCallClient client;
        CallController calls;
        FakeMediaBackend plain;
        calls.setClient(&client);
        QVERIFY(!calls.muteControlAvailable());
        calls.setMediaBackend(&plain);
        QVERIFY(calls.mediaBackendAvailable());
        QVERIFY2(!calls.muteControlAvailable(),
                 "a backend without mute support must not offer mute");
    }

    void ownRtcDeclineFromAnotherDeviceStopsTheRing()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);

        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = QStringLiteral("!r:x");
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = QStringLiteral("@peer:x");
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        client.emitSignal(notify);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        CallSignal decline;
        decline.kind = CallSignal::Kind::RtcDecline;
        decline.roomId = QStringLiteral("!r:x");
        decline.eventId = QStringLiteral("$decline-1");
        decline.sender = QStringLiteral("@me:x");
        decline.own = true;
        decline.targetEventId = QStringLiteral("$notify-1");
        client.emitSignal(decline);

        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(),
                 CallController::EndReason::DeclinedElsewhere);
        QVERIFY(client.sent.isEmpty());
    }

    void failedInviteDispatchEndsTheOutboundCall()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                         QStringLiteral("v=0 sdp")));
        QSignalSpy failed(&calls, &CallController::sendFailed);
        // The server refused the invite send.
        client.emitSendFinished(client.opCounter, false,
                                QStringLiteral("forbidden"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::SendFailed);
    }

    // The busy auto-reject is a remotely triggered send with no user action,
    // so it must be bounded.
    void busyRejectsAreBoundedPerSession()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-live")));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        for (int i = 0; i < 12; ++i) {
            client.emitSignal(freshInvite(
                QStringLiteral("flood-%1").arg(i),
                QStringLiteral("!other-%1:x").arg(i)));
        }
        int rejects = 0;
        for (const auto &event : client.sent)
            if (event.kind == QLatin1String("reject"))
                ++rejects;
        QCOMPARE(rejects, 8); // kMaxBusyRejectsPerSession
        // Session untouched throughout.
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeCallId(), QStringLiteral("call-live"));
    }

    // Re-delivery of the live session's own invite is idempotent; the busy
    // branch must not reject our own ring.
    void duplicateDeliveryOfLiveInviteIsIdempotent()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        const CallSignal invite = freshInvite(QStringLiteral("call-1"));
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeCallId(), QStringLiteral("call-1"));
        QVERIFY(client.sent.isEmpty());
    }

    // An ignored sender elicits nothing: no ring, no state and no reject (a
    // reject would confirm we are online during ignore propagation).
    void ignoredSenderElicitsNothing()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setSenderIgnoredCheck(
            [](const QString &sender) { return sender == QLatin1String("@peer:x"); });
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Idle);
        QVERIFY(client.sent.isEmpty());

        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = QStringLiteral("!r:x");
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = QStringLiteral("@peer:x");
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        client.emitSignal(notify);
        QCOMPARE(calls.state(), CallController::State::Idle);
        QVERIFY(client.sent.isEmpty());
    }

    // A stale send-op result from an ended call must not touch the next call.
    void staleOpFailureDoesNotTouchNewCall()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                         QStringLiteral("v=0 sdp")));
        const quint64 firstInviteOp = client.opCounter;
        QVERIFY(calls.hangup());
        QCOMPARE(calls.state(), CallController::State::Ended);

        QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                         QStringLiteral("v=0 sdp")));
        QCOMPARE(calls.state(), CallController::State::Inviting);
        QSignalSpy failed(&calls, &CallController::sendFailed);
        // The first call's invite send now reports failure.
        client.emitSendFinished(firstInviteOp, false,
                                QStringLiteral("network"));
        QCOMPARE(calls.state(), CallController::State::Inviting);
        QCOMPARE(failed.count(), 0);
    }

    // The targeted-invite filter resolves our identity from the client in the
    // production wiring, with no extra setter.
    void targetedInviteFilterUsesClientIdentity()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:x");
        CallController calls;
        calls.setClient(&client); // exactly what AppController wires
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        invite.invitee = QStringLiteral("@someone-else:x");
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Idle);
        QVERIFY(client.sent.isEmpty());

        CallSignal forUs = freshInvite(QStringLiteral("call-2"));
        forUs.invitee = QStringLiteral("@me:x");
        client.emitSignal(forUs);
        QCOMPARE(calls.state(), CallController::State::Ringing);
    }

    void placeCallWithBackendRunsTheFullOutboundCycle()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(client.mediaCapable); // registering enabled SDP transport

        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        QCOMPARE(calls.state(), CallController::State::Inviting);
        QCOMPARE(media.offerRequests.size(), 1);
        QVERIFY(client.sent.isEmpty()); // nothing on the wire pre-offer

        const QString callId = calls.activeCallId();
        media.deliverOffer(callId, QStringLiteral("v=0 offer"));
        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("invite"));

        // Peer answers; the bridge stored their answer SDP.
        CallSignal answer;
        answer.kind = CallSignal::Kind::Answer;
        answer.roomId = QStringLiteral("!r:x");
        answer.eventId = QStringLiteral("$answer-1");
        answer.sender = QStringLiteral("@peer:x");
        answer.callId = callId;
        answer.partyId = QStringLiteral("peer-party");
        client.storedDescriptions.insert(QStringLiteral("$answer-1"),
                                         QStringLiteral("v=0 answer"));
        client.emitSignal(answer);

        QCOMPARE(calls.state(), CallController::State::Connecting);
        QCOMPARE(media.remoteAnswers, QStringList{callId});
        QCOMPARE(media.lastRemoteAnswer, QStringLiteral("v=0 answer"));
        // select_answer was named exactly once.
        int selects = 0;
        for (const auto &event : client.sent)
            if (event.kind == QLatin1String("select_answer"))
                ++selects;
        QCOMPARE(selects, 1);

        media.deliverConnected(callId);
        QCOMPARE(calls.state(), CallController::State::Active);

        QVERIFY(calls.hangup());
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::LocalHangup);
        QCOMPARE(media.closed, QStringList{callId});
    }

    void answerRunsTheFullInboundCycle()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);

        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        client.storedDescriptions.insert(invite.eventId,
                                         QStringLiteral("v=0 remote-offer"));
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        QVERIFY(calls.answer());
        QCOMPARE(media.answerRequests, QStringList{QStringLiteral("call-1")});
        QCOMPARE(media.lastRemoteOffer, QStringLiteral("v=0 remote-offer"));

        media.deliverAnswer(QStringLiteral("call-1"),
                            QStringLiteral("v=0 our-answer"));
        QCOMPARE(calls.state(), CallController::State::Connecting);
        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("answer"));
        QVERIFY(!client.sent.first().partyId.isEmpty());

        media.deliverConnected(QStringLiteral("call-1"));
        QCOMPARE(calls.state(), CallController::State::Active);
    }

    void answerRefusesWithoutBackendOfferOrOnRtc()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        // No media backend.
        QVERIFY(!calls.answer());
        QCOMPARE(calls.lastRefusal(), QStringLiteral("no_media_backend"));

        FakeMediaBackend media;
        calls.setMediaBackend(&media);
        // Backend present but the bridge holds no remote offer (production
        // without media-capable mode, or the store already consumed).
        QVERIFY(!calls.answer());
        QCOMPARE(calls.lastRefusal(), QStringLiteral("no_remote_offer"));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(client.sent.isEmpty());
    }

    void rtcRingCannotBeAnswered()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = QStringLiteral("!r:x");
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = QStringLiteral("@peer:x");
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        client.emitSignal(notify);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(!calls.answer());
        QCOMPARE(calls.lastRefusal(), QStringLiteral("rtc_unsupported"));
    }

    void mediaFailureDuringInviteAnnouncesAndEnds()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        const QString callId = calls.activeCallId();

        // Failure BEFORE the offer: nothing reached the wire, so nothing
        // is announced.
        media.deliverFailure(callId, QStringLiteral("device"));
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::MediaFailed);
        QVERIFY(client.sent.isEmpty());
        QCOMPARE(media.closed, QStringList{callId});

        // Failure AFTER the invite went out: the peer is waiting, so a
        // user_media_failed hangup announces it.
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        const QString second = calls.activeCallId();
        media.deliverOffer(second, QStringLiteral("v=0 offer"));
        media.deliverFailure(second, QStringLiteral("device"));
        QCOMPARE(calls.endReason(), CallController::EndReason::MediaFailed);
        bool announced = false;
        for (const auto &event : client.sent)
            if (event.kind == QLatin1String("hangup")
                && event.callId == second
                && event.extra == QLatin1String("user_media_failed"))
                announced = true;
        QVERIFY(announced);
    }

    void staleMediaSignalsForOtherCallsAreIgnored()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        const QString callId = calls.activeCallId();
        // A late offer for a PREVIOUS call id must not dispatch an invite.
        media.deliverOffer(QStringLiteral("stale-call"),
                           QStringLiteral("v=0 stale"));
        QVERIFY(client.sent.isEmpty());
        QCOMPARE(calls.state(), CallController::State::Inviting);
        // And a connected() for another call must not activate this one.
        media.deliverOffer(callId, QStringLiteral("v=0 offer"));
        media.deliverConnected(QStringLiteral("stale-call"));
        QCOMPARE(calls.state(), CallController::State::Inviting);
    }

    void mediaProductionTimeoutEndsLocally()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        QCOMPARE(calls.state(), CallController::State::Inviting);
        // The backend never answers; drive the bounded production window's
        // expiry directly instead of waiting out the real 15s timer. The
        // session must end locally as MediaFailed with NO wire traffic —
        // in particular no invite_timeout hangup for an invite that was
        // never dispatched.
        QVERIFY(QMetaObject::invokeMethod(&calls, "onLifetimeExpired"));
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::MediaFailed);
        QVERIFY(client.sent.isEmpty());
    }

    void sdpStoreIsBoundedSingleShotAndClearable()
    {
        calls::SdpStore store;
        store.insert(QStringLiteral("$e1"), QStringLiteral("v=0 one"));
        // Single-shot: the second take is empty.
        QCOMPARE(store.take(QStringLiteral("$e1")), QStringLiteral("v=0 one"));
        QCOMPARE(store.take(QStringLiteral("$e1")), QString());
        // Empty ids/values are refused.
        store.insert(QString(), QStringLiteral("x"));
        store.insert(QStringLiteral("$e"), QString());
        QCOMPARE(store.size(), 0);
        // FIFO bound: the oldest entry is evicted past capacity.
        for (int i = 0; i < calls::SdpStore::kCapacity + 2; ++i)
            store.insert(QStringLiteral("$evt-%1").arg(i),
                         QStringLiteral("sdp-%1").arg(i));
        QCOMPARE(store.size(), calls::SdpStore::kCapacity);
        QCOMPARE(store.take(QStringLiteral("$evt-0")), QString());
        QCOMPARE(store.take(QStringLiteral("$evt-1")), QString());
        QVERIFY(!store.take(QStringLiteral("$evt-2")).isEmpty());
        // Re-inserting an existing id must not double-count in the order
        // list (otherwise the cap would evict early).
        store.clear();
        store.insert(QStringLiteral("$dup"), QStringLiteral("a"));
        store.insert(QStringLiteral("$dup"), QStringLiteral("b"));
        QCOMPARE(store.size(), 1);
        QCOMPARE(store.take(QStringLiteral("$dup")), QStringLiteral("b"));
        // clear() empties everything.
        store.insert(QStringLiteral("$x"), QStringLiteral("y"));
        store.clear();
        QCOMPARE(store.size(), 0);
    }

    void hangupEndsAnsweredInboundCall()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        client.storedDescriptions.insert(invite.eventId,
                                         QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        // Pre-answer, hangup still refuses: rejectIncoming is the honest
        // action for a ringing inbound call.
        QVERIFY(!calls.hangup());
        QVERIFY(calls.answer());
        media.deliverAnswer(QStringLiteral("call-1"),
                            QStringLiteral("v=0 answer"));
        QCOMPARE(calls.state(), CallController::State::Connecting);
        // Answered: the local user must be able to end their own call.
        QVERIFY(calls.hangup());
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::LocalHangup);
        bool hungUp = false;
        for (const auto &event : client.sent)
            if (event.kind == QLatin1String("hangup")
                && event.extra == QLatin1String("user_hangup"))
                hungUp = true;
        QVERIFY(hungUp);
    }

    void completedInboundCallIsNotMissed()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QSignalSpy ended(&calls, &CallController::incomingCallEnded);

        // Answered call, peer hangs up: reason RemoteHangup, missed FALSE.
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        client.storedDescriptions.insert(invite.eventId,
                                         QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QVERIFY(calls.answer());
        media.deliverAnswer(QStringLiteral("call-1"),
                            QStringLiteral("v=0 answer"));
        CallSignal hangup;
        hangup.kind = CallSignal::Kind::Hangup;
        hangup.roomId = QStringLiteral("!r:x");
        hangup.eventId = QStringLiteral("$hangup-1");
        hangup.sender = QStringLiteral("@peer:x");
        hangup.callId = QStringLiteral("call-1");
        hangup.partyId = QStringLiteral("peer-party");
        hangup.reason = QStringLiteral("user_hangup");
        client.emitSignal(hangup);
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.at(0).at(3).toBool(), false); // NOT missed

        // Pure ring the caller abandons: missed TRUE.
        client.emitSignal(freshInvite(QStringLiteral("call-2")));
        CallSignal hangup2 = hangup;
        hangup2.eventId = QStringLiteral("$hangup-2");
        hangup2.callId = QStringLiteral("call-2");
        client.emitSignal(hangup2);
        QCOMPARE(ended.count(), 2);
        QCOMPARE(ended.at(1).at(3).toBool(), true); // missed
    }

    void offerProductionPhaseNeverTouchesTheWire()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        // hangup() while the offer is still in production ends the call
        // locally and sends NOTHING — no peer was ever invited.
        QVERIFY(calls.hangup());
        QCOMPARE(calls.state(), CallController::State::Ended);
        QVERIFY(client.sent.isEmpty());

        // Glare against a pending-offer call retires ours with no wire hangup:
        // theirs wins and ours was never announced.
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        CallSignal theirs = freshInvite(QStringLiteral("0000-smaller"));
        client.emitSignal(theirs);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        for (const auto &event : client.sent)
            QVERIFY(event.kind != QLatin1String("hangup"));
    }

    void endedCallDropsItsUnconsumedOffer()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        client.storedDescriptions.insert(invite.eventId,
                                         QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QVERIFY(calls.rejectIncoming());
        // endSession issued the discard-take for the invite's event id.
        QVERIFY(client.takenDescriptions.contains(invite.eventId));
        QVERIFY(!client.storedDescriptions.contains(invite.eventId));
    }

    void incomingCallStartedCarriesTheRealRemainingLifetime()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        invite.lifetimeMs = 90000;
        client.emitSignal(invite);
        QCOMPARE(started.count(), 1);
        const qint64 remaining = started.at(0).at(3).toLongLong();
        QVERIFY(remaining > 80000);
        QVERIFY(remaining <= 90000);
    }

    // ICE candidates and TURN.

    void localCandidatesAreBatchedWithEndMarker()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        const QString callId = calls.activeCallId();
        media.deliverOffer(callId, QStringLiteral("v=0 offer"));

        media.deliverLocalCandidate(callId,
                                    QStringLiteral("candidate:0 1 UDP a"));
        media.deliverLocalCandidate(callId,
                                    QStringLiteral("candidate:1 1 UDP b"));
        QCOMPARE(client.candidateBatches.size(), 0); // batching window open
        QTest::qWait(250);
        QCOMPARE(client.candidateBatches.size(), 1);
        QCOMPARE(client.candidateBatches.first().second.size(), 2);

        // Gathering completion flushes immediately with the MSC2746 empty
        // end-of-candidates marker appended.
        media.deliverLocalCandidate(callId,
                                    QStringLiteral("candidate:2 1 UDP c"));
        media.deliverGatheringComplete(callId);
        QCOMPARE(client.candidateBatches.size(), 2);
        const QVariantList last = client.candidateBatches.last().second;
        QCOMPARE(last.size(), 2);
        QCOMPARE(last.last().toMap()
                     .value(QStringLiteral("candidate")).toString(),
                 QString());
    }

    void remoteCandidatesReachTheBackendForTheLiveCallOnly()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        // OUTBOUND call: the engine session exists from placeCall, so the
        // peer's candidates forward immediately (the inbound pre-answer
        // BUFFERING path has its own test).
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        const QString callId = calls.activeCallId();
        media.deliverOffer(callId, QStringLiteral("v=0 offer"));
        QVariantList batch;
        QVariantMap entry;
        entry.insert(QStringLiteral("candidate"),
                     QStringLiteral("candidate:0 1 UDP x"));
        batch.append(entry);
        // Wrong call id: dropped.
        client.emitCandidates(QStringLiteral("!r:x"),
                              QStringLiteral("other"), false, batch);
        QVERIFY(media.remoteCandidates.isEmpty());
        // Our own device's candidates: dropped.
        client.emitCandidates(QStringLiteral("!r:x"), callId, true, batch);
        QVERIFY(media.remoteCandidates.isEmpty());
        // The live call's: forwarded.
        client.emitCandidates(QStringLiteral("!r:x"), callId, false, batch);
        QCOMPARE(media.remoteCandidates.size(), 1);
    }

    void turnServersFlowToTheEngineOnceFetched()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        // Production order: AppController registers the engine before the
        // client, and the TURN pre-fetch must fire once both are set.
        calls.setMediaBackend(&media);
        QCOMPARE(client.lastTurnOp, quint64(0)); // no client yet: no fetch
        calls.setClient(&client);
        QVERIFY(client.lastTurnOp != 0);
        // A stale/foreign op id is ignored.
        client.emitTurnServers(client.lastTurnOp + 999, true,
                               { QStringLiteral("turn:one") });
        QVERIFY(media.iceServerApplications.isEmpty());
        client.emitTurnServers(client.lastTurnOp, true,
                               { QStringLiteral("turn:one"),
                                 QStringLiteral("stun:two") });
        QCOMPARE(media.iceServerApplications.size(), 1);
        QCOMPARE(media.iceServerApplications.first().size(), 2);
        // A fresh call within the TTL re-applies the cache, no new fetch.
        const quint64 fetchOp = client.lastTurnOp;
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        QCOMPARE(client.lastTurnOp, fetchOp);
        QCOMPARE(media.iceServerApplications.size(), 2);
    }

    void preAnswerCandidatesAreBufferedThenDrained()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        client.storedDescriptions.insert(invite.eventId,
                                         QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        // The caller trickles while we ring; the engine has no session yet,
        // so candidates are buffered, not dropped.
        QVariantList batch;
        for (int i = 0; i < 3; ++i) {
            QVariantMap entry;
            entry.insert(QStringLiteral("candidate"),
                         QStringLiteral("candidate:%1 1 UDP x").arg(i));
            batch.append(entry);
        }
        client.emitCandidates(QStringLiteral("!r:x"),
                              QStringLiteral("call-1"), false, batch);
        QVERIFY(media.remoteCandidates.isEmpty());

        QVERIFY(calls.answer());
        // Drained into the engine right after createAnswer.
        QCOMPARE(media.answerRequests.size(), 1);
        QCOMPARE(media.remoteCandidates.size(), 3);
    }

    void localCandidateFloodIsChunkedToTheWireCap()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        const QString callId = calls.activeCallId();
        media.deliverOffer(callId, QStringLiteral("v=0 offer"));
        // 40 candidates in one burst: the Rust side rejects more than 32 per
        // event, so the flush chunks (32 + 9 with the end marker).
        for (int i = 0; i < 40; ++i)
            media.deliverLocalCandidate(
                callId, QStringLiteral("candidate:%1 1 UDP x").arg(i));
        media.deliverGatheringComplete(callId);
        QCOMPARE(client.candidateBatches.size(), 2);
        QVERIFY(client.candidateBatches.at(0).second.size() <= 32);
        QVERIFY(client.candidateBatches.at(1).second.size() <= 32);
        int total = 0;
        for (const auto &batch : client.candidateBatches)
            total += batch.second.size();
        QCOMPARE(total, 41); // 40 + the end-of-candidates marker
    }

    void turnResponseIsBoundedDefensively()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setMediaBackend(&media);
        calls.setClient(&client);
        QVERIFY(client.lastTurnOp != 0);
        QStringList many;
        for (int i = 0; i < 40; ++i)
            many.append(QStringLiteral("turn:host%1:3478").arg(i));
        client.emitTurnServers(client.lastTurnOp, true, many);
        QCOMPARE(media.iceServerApplications.size(), 1);
        QCOMPARE(media.iceServerApplications.first().size(), 16); // capped
    }

    void loggedOutClearsSessionTimersAndOps()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        client.emitLoggedOut();
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::SessionLost);
        QVERIFY(!calls.sessionLive());

        // The retired-call LRU was cleared too: the same invite rings again
        // in the next session rather than being absorbed.
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
    }

    // Call stage data layer.

    void speakerUpdatesNeverResetTheParticipantModel()
    {
        // Speaker updates must not reset the participant model, which would
        // destroy every tile and VideoOutput several times a second.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_a"), false) }),
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"),
                           { sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_b"), false) }),
        });
        CallParticipantModel *model = call.participantModel();
        QVERIFY(model);
        QCOMPARE(model->rowCount(), 2);

        QSignalSpy resets(model, &QAbstractItemModel::modelAboutToBeReset);
        QSignalSpy removes(model, &QAbstractItemModel::rowsAboutToBeRemoved);
        QSignalSpy inserts(model, &QAbstractItemModel::rowsAboutToBeInserted);
        QSignalSpy changes(model, &QAbstractItemModel::dataChanged);

        for (int i = 0; i < 40; ++i) {
            const double level = (i % 2 == 0) ? 0.8 : 0.1;
            call.ingestSpeakersForTest(
                { speakerEntry(QStringLiteral("PA_1"), true, level) });
        }

        QCOMPARE(resets.count(), 0);
        QCOMPARE(removes.count(), 0);
        QCOMPARE(inserts.count(), 0);
        // The ring has something to animate: the level really did move.
        QVERIFY(changes.count() > 0);
        // ...and only the speaking roles moved with it.
        for (const QList<QVariant> &emission : changes) {
            const QList<int> roles = emission.at(2).value<QList<int>>();
            QVERIFY(!roles.isEmpty());
            for (int role : roles) {
                QVERIFY(role == CallParticipantModel::SpeakingRole
                        || role == CallParticipantModel::SpeakingLevelRole);
            }
        }
    }

    void speakingLevelCrossesFromTheSfuInsteadOfBeingThrownAway()
    {
        // LiveKit's SpeakerInfo `level` (0..1) reaches the model as
        // speakingLevel.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
        });
        call.ingestSpeakersForTest(
            { speakerEntry(QStringLiteral("PA_1"), true, 0.62) });

        CallParticipantModel *model = call.participantModel();
        const int row = participantRowFor(model, QStringLiteral("alice"));
        QCOMPARE(row, 0);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::SpeakingRole).toBool(),
                 true);
        QVERIFY(qFuzzyCompare(
            participantRole(model, row,
                            CallParticipantModel::SpeakingLevelRole)
                .toDouble() + 1.0,
            0.62 + 1.0));
    }

    void anSfuThatSendsOnlyActiveDegradesToABinaryRingNotADeadOne()
    {
        // A boolean is not turned into an amplitude: `speaking` is true so the
        // ring draws, `speakingLevel` stays 0.0 so it draws at its minimum.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
        });
        call.ingestSpeakersForTest(
            { speakerEntry(QStringLiteral("PA_1"), true) }); // no "level"

        CallParticipantModel *model = call.participantModel();
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::SpeakingRole).toBool(),
                 true);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::SpeakingLevelRole)
                     .toDouble(),
                 0.0);
    }

    void aSpeakerAbsentFromTheRoundStopsSpeaking()
    {
        // LiveKit sends the active set, so absence means stopped; reading
        // absence as "unchanged" leaves a ring stuck on.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
        });
        call.ingestSpeakersForTest(
            { speakerEntry(QStringLiteral("PA_1"), true, 0.5) });
        call.ingestSpeakersForTest({});

        CallParticipantModel *model = call.participantModel();
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::SpeakingRole).toBool(),
                 false);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::SpeakingLevelRole)
                     .toDouble(),
                 0.0);
    }

    void aMuteChangeIsOneRoleOnOneRowNotAMembershipChange()
    {
        // An update that changes a value must not look like a join.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_a"), false) }),
        });
        CallParticipantModel *model = call.participantModel();
        QSignalSpy resets(model, &QAbstractItemModel::modelAboutToBeReset);
        QSignalSpy inserts(model, &QAbstractItemModel::rowsAboutToBeInserted);
        QSignalSpy removes(model, &QAbstractItemModel::rowsAboutToBeRemoved);
        QSignalSpy changes(model, &QAbstractItemModel::dataChanged);

        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_a"), true) }),
        });

        QCOMPARE(resets.count(), 0);
        QCOMPARE(inserts.count(), 0);
        QCOMPARE(removes.count(), 0);
        QCOMPARE(changes.count(), 1);
        const QList<int> roles = changes.first().at(2).value<QList<int>>();
        QCOMPARE(roles,
                 QList<int>{ static_cast<int>(
                     CallParticipantModel::MicMutedRole) });
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::MicMutedRole).toBool(),
                 true);
    }

    void aParticipantLeavingIsARemoveNotAReset()
    {
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"), {}),
        });
        CallParticipantModel *model = call.participantModel();
        QSignalSpy resets(model, &QAbstractItemModel::modelAboutToBeReset);
        QSignalSpy removes(model, &QAbstractItemModel::rowsAboutToBeRemoved);

        QVariantMap gone = sfuParticipant(QStringLiteral("alice"),
                                          QStringLiteral("PA_1"), {});
        gone.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
        call.ingestParticipantsForTest({ gone });

        QCOMPARE(resets.count(), 0);
        QCOMPARE(removes.count(), 1);
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::IdentityRole)
                     .toString(),
                 QStringLiteral("bob"));
    }

    void twoSimultaneousScreenSharesAreTwoRows()
    {
        // Multiple simultaneous screen shares each get a row.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_b"), false) }),
        });
        CallShareModel *shares = call.shareModel();
        QVERIFY(shares);
        QCOMPARE(shares->rowCount(), 2);
        QCOMPARE(shares->shareIds(),
                 (QStringList{ QStringLiteral("TR_share_a"),
                               QStringLiteral("TR_share_b") }));
        // Distinct routing keys, so two surfaces can render at once.
        QCOMPARE(shares->get(0).value(QStringLiteral("trackKey")).toString(),
                 QStringLiteral("TR_share_a"));
        QCOMPARE(shares->get(1).value(QStringLiteral("trackKey")).toString(),
                 QStringLiteral("TR_share_b"));
    }

    void aDismissedShareStaysLiveAndIsAlwaysReachableAgain()
    {
        // Dismissal applies to the spotlight, never to the share's existence:
        // a closed share can always be brought back.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_b"), false) }),
        });
        CallStageState *stage = call.stageState();
        CallShareModel *shares = call.shareModel();
        QVERIFY(stage);
        // Newest share first: bob started last, so bob is on the spotlight.
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_b"));

        stage->dismissShare(QStringLiteral("TR_share_b"));
        // It fell through to the other share rather than nothing.
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_a"));
        QCOMPARE(shares->rowCount(), 2); // still live, still a grid tile

        stage->dismissShare(QStringLiteral("TR_share_a"));
        QCOMPARE(stage->spotlightShareId(), QString());
        // With nothing on the spotlight the shares are still rows, so a "show
        // it again" control has something to bind to.
        QCOMPARE(shares->rowCount(), 2);
        QCOMPARE(stage->restorableShareAvailable(), true);
        QCOMPARE(stage->dismissedShareCount(), 2);

        stage->restoreAllShares();
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_b"));
        QCOMPARE(stage->restorableShareAvailable(), false);
    }

    void aNewShareReArmsTheSpotlightAfterTheUserChoseGrid()
    {
        // The layout preference must not latch: a share that starts after the
        // user picked grid is not the one they dismissed.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        CallStageState *stage = call.stageState();
        stage->setLayoutPreference(QStringLiteral("grid"));
        QCOMPARE(stage->layoutPreference(), QStringLiteral("grid"));

        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_b"), false) }),
        });
        QCOMPARE(stage->layoutPreference(), QStringLiteral("auto"));
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_b"));
    }

    void anUnknownLayoutPreferenceIsRefusedRatherThanStored()
    {
        // An unrecognised mode is not read back verbatim.
        SfuCallController call;
        CallStageState *stage = call.stageState();
        stage->setLayoutPreference(QStringLiteral("spotlight"));
        stage->setLayoutPreference(QStringLiteral("nonsense"));
        QCOMPARE(stage->layoutPreference(), QStringLiteral("spotlight"));
    }

    void aRestartedShareIsOfferedAgainRatherThanInheritingADismissal()
    {
        // A restarted share is a new track and sid, and must not inherit the
        // dismissal.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_1"), false) }),
        });
        CallStageState *stage = call.stageState();
        stage->dismissShare(QStringLiteral("TR_share_1"));
        QCOMPARE(stage->spotlightShareId(), QString());

        // Alice stops sharing...
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_1"), true) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 0);
        QCOMPARE(stage->dismissedShareCount(), 0); // pruned with the share

        // ...and starts again. New track, new sid, offered.
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_2"), false) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 1);
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_2"));
        QCOMPARE(stage->isShareDismissed(QStringLiteral("TR_share_2")), false);
    }

    // A local stop clears the share row even while the server still reports
    // the track live: there is no unpublish message, so the server lags. Our
    // intent must override the stale report on the trailing edge too, or the
    // tile is never destroyed and the self-view freezes on its last frame.
    void aLocalStopClearsTheShareRowWhileTheServerStillReportsItLive()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("me"));
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_mine"), false) }),
        });
        call.setLocalMediaStateForTest(/*cameraOn=*/false,
                                       /*screenSharing=*/true);
        QCOMPARE(call.shareModel()->rowCount(), 1);

        // The user presses stop and the server says nothing new yet (the mute
        // must make a round trip).
        call.setLocalMediaStateForTest(/*cameraOn=*/false,
                                       /*screenSharing=*/false);
        QCOMPARE(call.shareModel()->rowCount(), 0);
    }

    // The stop must reach the SFU as a mute, or other clients keep being
    // offered a track that produces nothing.
    void aLocalStopTellsTheSfuTheScreenTrackIsMuted()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("me"));
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_mine"), false) }),
        });
        // Starting sends nothing: the server already reports it unmuted.
        call.setLocalMediaStateForTest(false, true);
        QCOMPARE(client.muteRequests.size(), 0);

        call.setLocalMediaStateForTest(false, false);
        QCOMPARE(client.muteRequests.size(), 1);
        QCOMPARE(client.muteRequests.at(0).first, QStringLiteral("TR_mine"));
        QCOMPARE(client.muteRequests.at(0).second, true);

        // It converges rather than firing once: until the report catches up a
        // later reconciliation re-sends the same request. It must never flip
        // direction.
        call.setLocalMediaStateForTest(false, false);
        QCOMPARE(client.muteRequests.size(), 2);
        QCOMPARE(client.muteRequests.at(1).first, QStringLiteral("TR_mine"));
        QCOMPARE(client.muteRequests.at(1).second, true);

        // It stops once the server agrees; the server's report is the state it
        // converges from, so it cannot loop.
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_mine"), true) }),
        });
        client.muteRequests.clear();
        call.setLocalMediaStateForTest(false, false);
        QCOMPARE(client.muteRequests.size(), 0);
    }

    // The camera has the same shape: a local stop turns the row off and sends
    // a mute.
    void aLocalCameraStopHasTheSameShape()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("me"));
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("camera"),
                                      QStringLiteral("TR_cam"), false) }),
        });
        call.setLocalMediaStateForTest(/*cameraOn=*/true, false);
        CallParticipantModel *model = call.participantModel();
        const int mine = participantRowFor(model, QStringLiteral("me"));
        QVERIFY(mine >= 0);
        QCOMPARE(participantRole(model, mine,
                                 CallParticipantModel::CameraOnRole).toBool(),
                 true);

        call.setLocalMediaStateForTest(/*cameraOn=*/false, false);
        QCOMPARE(participantRole(model, mine,
                                 CallParticipantModel::CameraOnRole).toBool(),
                 false);
        QCOMPARE(client.muteRequests.size(), 1);
        QCOMPARE(client.muteRequests.at(0).first, QStringLiteral("TR_cam"));
        QCOMPARE(client.muteRequests.at(0).second, true);
    }

    // An SFU identity that resolves to no Matrix device leaves that joiner
    // unkeyed and inaudible, so it is logged once per identity. Only the
    // no-session branch is driven here; both branches share the reporter.
    void anSfuIdentityThatResolvesToNobodySaysSoExactlyOnce()
    {
        // RAII, because QVERIFY returns from the slot: a handler not restored
        // on failure points at a destroyed stack local.
        struct Capture {
            static QStringList *&sink()
            {
                static QStringList *s = nullptr;
                return s;
            }
            explicit Capture(QStringList *lines)
            {
                sink() = lines;
                m_previous = qInstallMessageHandler(
                    [](QtMsgType, const QMessageLogContext &,
                       const QString &m) {
                        if (QStringList *s = sink())
                            s->append(m);
                    });
            }
            ~Capture()
            {
                qInstallMessageHandler(m_previous);
                sink() = nullptr;
            }
            QtMessageHandler m_previous = nullptr;
        };

        QStringList lines;
        RtcController rtc;
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("PA_nobody");

        const auto countSaid = [&lines, &identity] {
            int n = 0;
            for (const QString &line : lines) {
                if (line.contains(QLatin1String("could NOT be resolved"))
                    && line.contains(identity)) {
                    ++n;
                }
            }
            return n;
        };

        // A miss at join is not a fault: a client reaches the SFU before its
        // own membership comes back through sync, so early lookups resolve
        // nothing, including the local device.
        {
            Capture capture(&lines);
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
        }
        QVERIFY2(countSaid() == 0,
                 "a transient lookup miss must not be reported as a "
                 "permanent one");

        // A fault that persists is still reported, exactly once. The grace is
        // collapsed rather than slept through.
        lines.clear();
        rtc.setUnresolvedIdentityGraceMsForTest(0);
        {
            Capture capture(&lines);
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
        }
        QVERIFY2(countSaid() == 1,
                 "an identity that stays unresolvable must be reported, and "
                 "reported once");

        // A new call diagnoses itself again; SFU identities are stable per user
        // and device.
        lines.clear();
        rtc.forgetUnresolvedIdentityDiagnostics();
        {
            Capture capture(&lines);
            // Twice: forgetting clears the first-seen record too, so the
            // grace starts again even at zero.
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
        }
        int again = 0;
        for (const QString &line : lines) {
            if (line.contains(QLatin1String("could NOT be resolved"))
                && line.contains(identity)) {
                ++again;
            }
        }
        QCOMPARE(again, 1);
    }

    void theRefreshTickReconcilesTheKeyLaneNotJustTheMembership()
    {
        const QString room = QStringLiteral("!room:example.org");

        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMembershipForTest(room, QString());
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));

        // Outside a call the tick must do nothing: no key to hold, nobody to
        // address, so a reconcile would be noise.
        call.setCallStateForTest(SfuCallController::State::Idle);
        const int idleBefore = call.keyLaneReconcilesForTest();
        QMetaObject::invokeMethod(&call, "reconcileKeyLane");
        QCOMPARE(call.keyLaneReconcilesForTest(), idleBefore);

        call.setCallStateForTest(SfuCallController::State::Connected);
        QMetaObject::invokeMethod(&call, "reconcileKeyLane");
        QCOMPARE(call.keyLaneReconcilesForTest(), idleBefore + 1);

        // The real refresh timer must drive the key-lane reconcile:
        // sessionChanged is suppressed for an unchanged membership read, so a
        // failed distribution would otherwise never retry.
        const int before = call.keyLaneReconcilesForTest();
        call.startRefreshTickForTest(1);
        QTRY_VERIFY2(call.keyLaneReconcilesForTest() > before,
                     "the refresh tick must reconcile the key lane, or a "
                     "failed key distribution is only retried when somebody "
                     "joins or leaves");
    }

    void aMembershipArrivingAfterTheSfuStillNamesTheJoiner()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");

        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));

        // The SFU announces a joiner before their membership comes back down
        // sync; the row must be rebuilt when it lands. First, an anonymous row.
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_BEA"), {}),
        });
        CallParticipantModel *model = call.participantModel();
        const int row = participantRowFor(model, identity);
        QVERIFY2(row >= 0, "the SFU's announcement did not produce a row");
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::DisplayNameRole)
                     .toString(),
                 QString());

        // ...and now the membership lands.
        RtcParticipant bea;
        bea.userId = QStringLiteral("@bea:example.org");
        bea.deviceId = QStringLiteral("BDEV");
        bea.rtcIdentity = identity;
        bea.intent = QStringLiteral("audio");
        bea.displayName = QStringLiteral("Bea");
        bea.avatarMxc = QStringLiteral("mxc://example.org/bea");
        bea.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = room;
        session.participants = { bea };

        rtc.refresh(room);
        QCOMPARE(client.sessionReads.size(), 1);
        client.answerSession(client.lastSessionOp, session);

        const int after = participantRowFor(model, identity);
        QVERIFY(after >= 0);
        QCOMPARE(participantRole(model, after,
                                 CallParticipantModel::DisplayNameRole)
                     .toString(),
                 QStringLiteral("Bea"));
        QCOMPARE(participantRole(model, after,
                                 CallParticipantModel::AvatarMxcRole)
                     .toString(),
                 QStringLiteral("mxc://example.org/bea"));
        QCOMPARE(participantRole(model, after,
                                 CallParticipantModel::UserIdRole).toString(),
                 QStringLiteral("@bea:example.org"));
    }

    // Restarting a share must not unmute the stopped track. A stop is a mute,
    // so our row still lists the old track, and its server-assigned sid cannot
    // be mapped to the new one; the video path therefore only ever mutes. A
    // new track is published fresh and reported unmuted anyway.
    void aRestartedLocalShareNeverUnmutesTheTrackItJustStopped()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("me"));
        // The stopped share, still listed because a mute removes nothing.
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_dead"), true) }),
        });
        // A new share whose track has not been announced yet.
        call.setLocalMediaStateForTest(/*cameraOn=*/false,
                                       /*screenSharing=*/true);
        QCOMPARE(client.muteRequests.size(), 0);

        // The microphone genuinely needs unmutes and never accumulates tracks.
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_dead"), true),
                             sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_mic"), true) }),
        });
        call.setMicrophoneMuted(true);
        client.muteRequests.clear();
        call.setMicrophoneMuted(false);
        QCOMPARE(client.muteRequests.size(), 1);
        QCOMPARE(client.muteRequests.at(0).first, QStringLiteral("TR_mic"));
        QCOMPARE(client.muteRequests.at(0).second, false);
    }

    // A restarted share routes to the live track, not the muted one: our row
    // can carry two tracks of one source, so the first match is wrong.
    void aRestartedShareRoutesToTheLiveTrackNotTheMutedOne()
    {
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_dead"), true),
                             sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_live"), false) }),
        });
        CallParticipantModel *model = call.participantModel();
        const int row = participantRowFor(model, QStringLiteral("alice"));
        QVERIFY(row >= 0);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::ScreenSharingRole)
                     .toBool(),
                 true);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::ScreenTrackKeyRole)
                     .toString(),
                 QStringLiteral("TR_live"));
        QCOMPARE(call.shareModel()->rowCount(), 1);
    }

    void handRaiseIsLocalOnlyAndSaysSoOnEveryOtherRow()
    {
        // Raised hand is local feedback only in this role; the badge must
        // light for the local user.
        SfuCallController call;
        call.setOwnIdentityForTest(QStringLiteral("me"));
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"), {}),
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
        });
        call.setHandRaised(true);

        CallParticipantModel *model = call.participantModel();
        const int mine = participantRowFor(model, QStringLiteral("me"));
        const int theirs = participantRowFor(model, QStringLiteral("alice"));
        QVERIFY(mine >= 0);
        QVERIFY(theirs >= 0);
        QCOMPARE(participantRole(model, mine,
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 true);
        QCOMPARE(participantRole(model, theirs,
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 false);
    }

    void localVolumeIsReadableBackFromTheModel()
    {
        // participantVolume is readable, so a slider can show the value it set.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
        });
        CallParticipantModel *model = call.participantModel();
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 100);
        call.setParticipantVolume(QStringLiteral("alice"), 40);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 40);
        // Clamped to the real ceiling: 200 is a deliberate boost range, so an
        // over-range value saturates at 200 rather than snapping to unity.
        call.setParticipantVolume(QStringLiteral("alice"), 400);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);
        // The top of the range is reachable (catches an off-by-one at 199).
        call.setParticipantVolume(QStringLiteral("alice"), 200);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);
        // And the floor: negative is silence, never a wrap or a reset.
        call.setParticipantVolume(QStringLiteral("alice"), -25);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 0);
    }

    void connectionQualityIsMergedAndUnknownIsNeverRendered()
    {
        // sfuConnectionQuality reaches the participant model.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"), {}),
        });
        QVariantMap good;
        good.insert(QStringLiteral("sid"), QStringLiteral("PA_1"));
        good.insert(QStringLiteral("quality"), QStringLiteral("excellent"));
        QVariantMap unknown;
        unknown.insert(QStringLiteral("sid"), QStringLiteral("PA_2"));
        unknown.insert(QStringLiteral("quality"), QStringLiteral("unknown"));
        call.ingestConnectionQualityForTest({ good, unknown });

        CallParticipantModel *model = call.participantModel();
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::ConnectionQualityRole)
                     .toString(),
                 QStringLiteral("excellent"));
        // "unknown" is the default, not a value to draw a badge for.
        QCOMPARE(participantRole(model, 1,
                                 CallParticipantModel::ConnectionQualityRole)
                     .toString(),
                 QString());

        // A round that does not mention a sid is a delta: the last value
        // survives.
        call.ingestConnectionQualityForTest({ unknown });
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::ConnectionQualityRole)
                     .toString(),
                 QStringLiteral("excellent"));
    }

    void leavingClearsTheStageStateSoTheNextCallStartsClean()
    {
        // Stage view state lives in C++ so it survives the QML Loader a room
        // switch destroys; therefore leaving the call must clear it.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        CallStageState *stage = call.stageState();
        stage->pin(QStringLiteral("alice"));
        stage->dismissShare(QStringLiteral("TR_share_a"));
        stage->setLayoutPreference(QStringLiteral("grid"));
        QCOMPARE(call.participantModel()->rowCount(), 1);

        call.leave();

        QCOMPARE(call.participantModel()->rowCount(), 0);
        QCOMPARE(call.shareModel()->rowCount(), 0);
        QCOMPARE(call.participantCount(), 0);
        QCOMPARE(stage->pinnedIdentity(), QString());
        QCOMPARE(stage->layoutPreference(), QStringLiteral("auto"));
        QCOMPARE(stage->dismissedShareCount(), 0);
        QCOMPARE(stage->spotlightShareId(), QString());
    }

    void participantsInvokableIsReadOutOfTheModel()
    {
        // One derivation: the legacy participants() invokable must agree with
        // the model.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_a"), true) }),
        });
        const QVariantList rows = call.participants();
        QCOMPARE(rows.size(), 1);
        const QVariantMap row = rows.first().toMap();
        QCOMPARE(row.value(QStringLiteral("identity")).toString(),
                 QStringLiteral("alice"));
        QCOMPARE(row.value(QStringLiteral("micKnown")).toBool(), true);
        QCOMPARE(row.value(QStringLiteral("micMuted")).toBool(), true);
        // The keys the existing surfaces read are all still there.
        for (const char *key : { "identity", "userId", "displayName",
                                 "avatarMxc", "local", "speaking",
                                 "micKnown", "micMuted", "cameraKnown",
                                 "cameraOn", "screenSharing",
                                 "cameraTrackKey", "screenTrackKey" }) {
            QVERIFY2(row.contains(QLatin1String(key)), key);
        }
        QCOMPARE(call.participantCount(), 1);
    }

    void theParticipantCountNotifiesOnEveryPathThatMovesIt()
    {
        // `participantCount` reads the model's rowCount, so its NOTIFY must
        // fire on every path that rebuilds the model. Asserted through the
        // property's own notify signal.
        SfuCallController call;
        const QMetaObject *mo = call.metaObject();
        const int idx = mo->indexOfProperty("participantCount");
        QVERIFY(idx >= 0);
        const QMetaProperty prop = mo->property(idx);
        QVERIFY(prop.hasNotifySignal());
        QSignalSpy notified(&call, prop.notifySignal());

        QCOMPARE(call.participantCount(), 0);
        call.setOwnIdentityForTest(QStringLiteral("PA_me"));

        QCOMPARE(call.participantCount(), 1);
        QVERIFY2(notified.count() >= 1,
                 "participantCount moved without notifying");
    }

    void fullScreenIsRefusedWhenThereIsNothingToShow()
    {
        // Full screen must never show an empty rectangle. The guard lives in
        // the state object so every caller inherits it and it can be driven.
        SfuCallController call;
        CallStageState *stage = call.stageState();
        QVERIFY(stage);
        QCOMPARE(stage->fullScreen(), false);

        // Nothing focused: refused, not stored.
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), false);

        // A live share gives it something to show.
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_a"));
        QSignalSpy fullScreenSpy(stage, &CallStageState::fullScreenChanged);
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
        QCOMPARE(fullScreenSpy.count(), 1);

        // A pin is also something to show, with no share at all.
        stage->setFullScreen(false);
        stage->dismissShare(QStringLiteral("TR_share_a"));
        QCOMPARE(stage->spotlightShareId(), QString());
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), false); // nothing focused yet
        stage->pin(QStringLiteral("alice"));
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
    }

    // Picture-in-picture: the flag lives in the state object so the call's
    // lifecycle can drop it.
    void pictureInPictureAndFullScreenAreMutuallyExclusive()
    {
        // SfuVideoRouter holds one sink per track and the last attach wins, so
        // two surfaces on one participant leave one black. Enforced here
        // rather than hoping no QML site opens both.
        SfuCallController call;
        CallStageState *stage = call.stageState();
        QVERIFY(stage);
        QCOMPARE(stage->pictureInPicture(), false);

        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);

        QSignalSpy pipSpy(stage, &CallStageState::pictureInPictureChanged);
        QSignalSpy fsSpy(stage, &CallStageState::fullScreenChanged);
        stage->setPictureInPicture(true);
        QCOMPARE(stage->pictureInPicture(), true);
        QCOMPARE(pipSpy.count(), 1);
        QVERIFY2(!stage->fullScreen(),
                 "entering picture-in-picture must leave full screen: both "
                 "render the focused surface and only one can hold its sink");
        QCOMPARE(fsSpy.count(), 1);

        // Idempotent, like every other flag here.
        stage->setPictureInPicture(true);
        QCOMPARE(pipSpy.count(), 1);
    }

    void aCallEndingTakesTheFloatingWindowWithIt()
    {
        // A floating window for an ended call would show a frozen frame and
        // need dismissing by hand; same rule as full screen.
        SfuCallController call;
        CallStageState *stage = call.stageState();
        QVERIFY(stage);
        stage->setPictureInPicture(true);
        QCOMPARE(stage->pictureInPicture(), true);

        QSignalSpy pipSpy(stage, &CallStageState::pictureInPictureChanged);
        stage->clear();
        QCOMPARE(stage->pictureInPicture(), false);
        QCOMPARE(pipSpy.count(), 1);
    }

    // Unlike full screen, picture-in-picture does not need a focused surface:
    // it is most useful on a voice-only call. The rules differ on purpose.
    void pictureInPictureIsOfferedOnAVoiceOnlyCall()
    {
        SfuCallController call;
        CallStageState *stage = call.stageState();
        QVERIFY(stage);
        QCOMPARE(stage->spotlightShareId(), QString());
        QCOMPARE(stage->pinnedIdentity(), QString());
        // Full screen refuses this exact state...
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), false);
        // ...and picture-in-picture accepts it.
        stage->setPictureInPicture(true);
        QCOMPARE(stage->pictureInPicture(), true);
    }

    void fullScreenDropsItselfWhenTheFocusedSurfaceGoesAway()
    {
        // When the share ends or the pin drops, full screen falls by itself;
        // the QML window is driven from the flag.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        CallStageState *stage = call.stageState();
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);

        // The sharer stops: the share row goes, and with it full screen.
        QVariantMap stopped = sfuParticipant(QStringLiteral("alice"),
                                             QStringLiteral("PA_1"),
                                             { sfuTrack(
                                                 QStringLiteral("screen_share"),
                                                 QStringLiteral("TR_share_a"),
                                                 true) });
        call.ingestParticipantsForTest({ stopped });
        QCOMPARE(stage->spotlightShareId(), QString());
        QCOMPARE(stage->fullScreen(), false);

        // Same for a pin that is dropped by "Back to grid".
        stage->pin(QStringLiteral("alice"));
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
        stage->clearPin();
        QCOMPARE(stage->fullScreen(), false);

        // And leaving the call clears it whatever it was.
        stage->pin(QStringLiteral("alice"));
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
        call.leave();
        QCOMPARE(stage->fullScreen(), false);
    }

    // Leaving the call.

    // The retraction is issued and names the room and the delay id the
    // membership was published with.
    void leavingRetractsTheMembershipItPublished()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QStringLiteral("delay-1"));

        call.leave();

        QCOMPARE(client.retractions.size(), 1);
        QCOMPARE(client.retractions.first().first,
                 QStringLiteral("!room:example.org"));
        QCOMPARE(client.retractions.first().second,
                 QStringLiteral("delay-1"));
    }

    // A failed retraction is retried with backoff, then given up on loudly;
    // otherwise the membership stays in the room.
    void aFailedRetractionIsRetriedAndThenGivenUpOnLoudly()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QStringLiteral("delay-1"));
        call.leave();
        QCOMPARE(client.retractions.size(), 1);

        // Offline at hang-up. The retry is backed off, so the wait covers it.
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QTRY_COMPARE_WITH_TIMEOUT(client.retractions.size(), 2, 5000);
        QCOMPARE(client.retractions.at(1).first,
                 QStringLiteral("!room:example.org"));
        QCOMPARE(client.retractions.at(1).second, QStringLiteral("delay-1"));

        // Bounded: leaving must not become an unbounded background sender.
        for (int i = 0; i < 8; ++i) {
            client.answerMembershipOp(client.lastRetractOp, false,
                                      QStringLiteral("network"));
            QTest::qWait(120);
        }
        QVERIFY2(client.retractions.size() <= 4,
                 "the retry must be bounded, not a permanent sender");
    }

    // A permanent refusal (`forbidden`) is not retried.
    void aRetractionRefusedOnPolicyGroundsIsNotRetried()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QStringLiteral("delay-1"));
        call.leave();
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("forbidden"));
        // Longer than the first retry delay, so this proves no retry was armed.
        QTest::qWait(2600);
        QCOMPARE(client.retractions.size(), 1);
    }

    // A successful retraction stops the retry machinery.
    void anAcknowledgedRetractionIsNotRepeated()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QStringLiteral("delay-1"));
        call.leave();
        client.answerMembershipOp(client.lastRetractOp, true, QString());
        QTest::qWait(2600);
        QCOMPARE(client.retractions.size(), 1);
    }

    // The destructor retracts too (application quit reaches it; AppController
    // should still call leave() explicitly).
    void destroyingTheControllerRetracts()
    {
        RecordingCallClient client;
        {
            SfuCallController call;
            call.setClient(&client);
            call.setCallStateForTest(SfuCallController::State::Connected);
            call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                      QStringLiteral("delay-1"));
        }
        QCOMPARE(client.retractions.size(), 1);
        QCOMPARE(client.retractions.first().first,
                 QStringLiteral("!room:example.org"));
    }

    // Without an MSC4140 delayed event (off by default in Synapse) the
    // membership's `expires` is the only cleanup, so Rust publishes a short
    // one and the heartbeat must re-publish to keep a live participant.
    void withoutADelayedRetractionTheHeartbeatRePublishesTheMembership()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());

        // Invoke the heartbeat's own slot, as the timer does.
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.publishes.size(), 1);
        QCOMPARE(client.publishes.first(), QStringLiteral("!room:example.org"));
        // Nothing is asked of a delayed event that does not exist.
        QCOMPARE(client.delayedRestarts.size(), 0);

        // Re-publish on a one-minute cadence, not every 5 s tick.
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.publishes.size(), 1);
    }

    // With a delayed event the heartbeat restarts it and does not rewrite the
    // state event.
    void withADelayedRetractionTheHeartbeatOnlyRestartsIt()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QStringLiteral("delay-1"));

        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.delayedRestarts.size(), 1);
        QCOMPARE(client.delayedRestarts.first(), QStringLiteral("delay-1"));
        QCOMPARE(client.publishes.size(), 0);
    }

    // A failed restart may mean the server already fired the delayed
    // retraction and consumed the id; repair by re-publishing (arming a fresh
    // one), never by restarting the dead id.
    void aFailedDelayedRestartRePublishesInsteadOfRestartingADeadId()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QStringLiteral("delay-1"));

        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.delayedRestarts.size(), 1);
        client.answerMembershipOp(client.lastRestartOp, false,
                                  QStringLiteral("not_found"));
        QCOMPARE(client.publishes.size(), 1);

        // The re-publish's new delay id is the one the heartbeat uses from now on.
        client.answerPublish(client.lastPublishOp, true,
                             QStringLiteral("delay-2"));
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.delayedRestarts.size(), 2);
        QCOMPARE(client.delayedRestarts.at(1), QStringLiteral("delay-2"));
    }

    // A refresh answer must not re-run the join sequence; first publish and
    // refreshes share `rtcMembershipPublished` and differ only by op id.
    void aRefreshAnswerDoesNotRestartTheJoin()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        client.answerPublish(client.lastPublishOp, true, QString());
        // Still in the call, same room, no second SFU connect.
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Connected));
        QCOMPARE(call.roomId(), QStringLiteral("!room:example.org"));
    }

    // An empty delay id has two meanings: `unrecognized`/`not_found`/
    // `no_delay_id` mean no usable MSC4140 endpoint (cleanup rests on
    // `expires`); anything else is one refused write. The reason is kept.
    void aRefusedDelayedRetractionKeepsItsReason()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        QVERIFY(call.delayedRefusalReason().isEmpty());

        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        client.answerPublish(client.lastPublishOp, true, QString(),
                             QStringLiteral("unrecognized"));
        QCOMPARE(call.delayedRefusalReason(),
                 QStringLiteral("unrecognized"));

        // A later publish that arms one clears it, matching rtc.rs's re-probe.
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        client.answerPublish(client.lastPublishOp, true,
                             QStringLiteral("delay-9"));
        QVERIFY(call.delayedRefusalReason().isEmpty());
    }

    // `forbidden` from the homeserver (our membership state event: a room
    // power-level problem) and from the SFU (the call service's own
    // authorisation) need different messages; only the first points at room
    // permissions.
    void aHomeserverRefusingOurMembershipIsNotTheCallServiceRefusingTheCall()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        // (a) The homeserver refuses the membership state event.
        const quint64 publish = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        QVERIFY(publish != 0);
        client.refusePublish(publish, QStringLiteral("forbidden"));
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Failed));
        const QString roomRefusal = call.lastError();
        QVERIFY(!roomRefusal.isEmpty());

        // (b) The call service refuses the connection after the membership was
        // accepted.
        const quint64 second = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        client.answerPublish(second, true, QStringLiteral("delay-1"));
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Authorizing));
        client.emitSfuState(QStringLiteral("failed"),
                            QStringLiteral("forbidden"));
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Failed));
        const QString serviceRefusal = call.lastError();
        QVERIFY(!serviceRefusal.isEmpty());

        QVERIFY2(roomRefusal != serviceRefusal,
                 "both refusals still say the same sentence, so nobody can "
                 "tell a room permission problem from the call service "
                 "refusing the connection");
        // The membership refusal points at the room. Asserted on the remedy
        // words so rewording stays free.
        QVERIFY2(roomRefusal.contains(QStringLiteral("permission"))
                     && roomRefusal.contains(QStringLiteral("room")),
                 qPrintable(QStringLiteral(
                                "the membership refusal must point at the "
                                "room's permissions; it said: %1")
                                .arg(roomRefusal)));
        // The service refusal must not, or it sends the user to a room admin.
        QVERIFY2(!serviceRefusal.contains(QStringLiteral("permission")),
                 qPrintable(QStringLiteral(
                                "the call service's refusal must not read as "
                                "a room permission problem; it said: %1")
                                .arg(serviceRefusal)));
    }

    // A refusal is withdrawn when a later attempt gets past the gate that
    // refused it; `callFailed` is otherwise a one-shot notice nothing clears.
    void aLaterSuccessfulJoinWithdrawsTheRefusalItReplaced()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        QSignalSpy failures(&call, &SfuCallController::callFailed);

        const quint64 refused = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        client.refusePublish(refused, QStringLiteral("forbidden"));
        QCOMPARE(failures.size(), 1);
        QVERIFY(!failures.at(0).at(0).toString().isEmpty());

        // Same room, and this time the homeserver accepts the membership.
        const quint64 accepted = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        client.answerPublish(accepted, true, QStringLiteral("delay-1"));
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Authorizing));

        QVERIFY2(failures.size() == 2,
                 "the join that succeeded never withdrew the refusal it "
                 "replaced, so the status strip goes on showing it");
        QVERIFY2(failures.at(1).at(0).toString().isEmpty(),
                 "the withdrawal must be an EMPTY message — that is what "
                 "clears a reported error; anything else is a second error");
        // Withdrawn once, not on every later state change.
        client.emitSfuState(QStringLiteral("signalling"), QString());
        QCOMPARE(failures.size(), 2);
    }

    // The withdrawal does not fire at Preparing, so a retry refused by the
    // same gate does not blink the message off and on.
    void aRetryThatIsRefusedAgainNeverBlanksTheReasonInBetween()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        QSignalSpy failures(&call, &SfuCallController::callFailed);

        const quint64 first = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        client.refusePublish(first, QStringLiteral("forbidden"));
        QCOMPARE(failures.size(), 1);

        // The retry reaches Preparing and is refused there too.
        const quint64 second = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Preparing));
        QVERIFY2(failures.size() == 1,
                 "reaching Preparing withdrew the refusal, so the reason "
                 "blanks the moment Join is pressed and comes straight back "
                 "when the same gate refuses again");
        client.refusePublish(second, QStringLiteral("forbidden"));
        QCOMPARE(failures.size(), 2);
        QVERIFY2(!failures.at(1).at(0).toString().isEmpty(),
                 "a retry that was refused again must report the refusal, "
                 "never withdraw it");
    }

    // A participant volume survives a restart: two controllers over one
    // SettingsManager, the second knowing only what reached the store.
    void aParticipantVolumeSurvivesARestart()
    {
        const QString room = QStringLiteral("!vol:example.org");
        const QString identity = QStringLiteral("@her:example.org:HERDEV");
        const QString userId = QStringLiteral("@her:example.org");

        // No secret store: the volume key is scoped by the account record,
        // not the token.
        SettingsManager settings;
        settings.saveSession(QStringLiteral("https://example.org"),
                             QStringLiteral("@me:example.org"),
                             QStringLiteral("MEDEV"),
                             QStringLiteral("token-fixture"));

        // The store outlives the process; reset so a stale value cannot pass.
        settings.setCallParticipantVolume(userId, 100);
        QCOMPARE(settings.callParticipantVolume(userId), 100);

        {
            RecordingCallClient client;
            RtcController rtc;
            SfuCallController call;
            call.setSettings(&settings);
            CallParticipantModel *model = stageOneRemoteParticipant(
                client, rtc, call, room, identity, userId,
                QStringLiteral("HERDEV"), QStringLiteral("$m1"));
            QVERIFY(model != nullptr);
            call.setParticipantVolume(identity, 40);
            QCOMPARE(call.participantVolume(identity), 40);
        }

        // Both halves share one SettingsManager, so QSettings' cache could
        // answer; reaching disk is proven by SettingsSessionTest.
        RecordingCallClient client2;
        RtcController rtc2;
        SfuCallController call2;
        call2.setSettings(&settings);
        CallParticipantModel *model2 = stageOneRemoteParticipant(
            client2, rtc2, call2, room, identity, userId,
            QStringLiteral("HERDEV"), QStringLiteral("$m2"));
        QVERIFY(model2 != nullptr);

        QCOMPARE(call2.participantVolume(identity), 40);
        QCOMPARE(participantRole(model2, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 40);
    }

    // A stored level must reach the engine when the stream id arrives late.
    // The row appears first and the model takes the stored level with nothing
    // to address; a later pass must not skip because the model already
    // matches. Asserted on the engine record
    // (engineParticipantVolumeForTest), since participantVolume() falls back
    // to the store and reads 200 even on broken code.
    void aStoredVolumeReachesTheEngineWhenTheStreamIdArrivesLate()
    {
        const QString room = QStringLiteral("!late:example.org");
        const QString identity = QStringLiteral("@her:example.org:HERDEV");
        const QString userId = QStringLiteral("@her:example.org");

        SettingsManager settings;
        settings.saveSession(QStringLiteral("https://example.org"),
                             QStringLiteral("@me:example.org"),
                             QStringLiteral("MEDEV"),
                             QStringLiteral("token-fixture"));
        // The store outlives the process, so prove the fixture first.
        settings.setCallParticipantVolume(userId, 100);
        QCOMPARE(settings.callParticipantVolume(userId), 100);
        settings.setCallParticipantVolume(userId, 200);

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        call.setSettings(&settings);
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));

        RtcParticipant member;
        member.userId = userId;
        member.deviceId = QStringLiteral("HERDEV");
        member.rtcIdentity = identity;
        member.intent = QStringLiteral("audio");
        member.membershipEventId = QStringLiteral("$m1");
        member.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = room;
        session.participants = { member };
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);

        // 1. The row, with no stream id to address.
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QString(), {}),
        });
        const int row = participantRowFor(call.participantModel(), identity);
        QVERIFY2(row >= 0, "the participant row never appeared, so this case "
                           "cannot be driving the ordering it is about");
        QCOMPARE(participantRole(call.participantModel(), row,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);
        QCOMPARE(call.engineParticipantVolumeForTest(identity), -1);

        // 2. The stream id arrives.
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_1"), false) }),
        });
        QCOMPARE(participantRole(call.participantModel(), row,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);
        QVERIFY2(call.engineParticipantVolumeForTest(identity) != -1,
                 "the stored level never reached the audio graph: the model "
                 "and the store agree, the slider reads 200%, and the "
                 "element was never told");
        QCOMPARE(call.engineParticipantVolumeForTest(identity), 200);

        // The getter reads 200 regardless; asserted so nobody rewrites the
        // case around it.
        QCOMPARE(call.participantVolume(identity), 200);
    }

    // A share level changed in Settings reaches a live call through its own
    // signal (`callShareVolumeChanged`), without a participant change first
    // and without duplicate connections. Asserted on the engine record, not
    // shareVolume(), which reads the store.
    void aShareLevelChangedInSettingsReachesTheCallOnItsOwnSignal()
    {
        const QString room = QStringLiteral("!shsig:example.org");
        const QString identity = QStringLiteral("@her:example.org:HERDEV");
        const QString userId = QStringLiteral("@her:example.org");

        SettingsManager settings;
        settings.saveSession(QStringLiteral("https://example.org"),
                             QStringLiteral("@me:example.org"),
                             QStringLiteral("MEDEV"),
                             QStringLiteral("token-fixture"));
        settings.setCallShareVolume(userId, 100);
        QCOMPARE(settings.callShareVolume(userId), 100);

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        call.setSettings(&settings);
        QVERIFY(stageOneRemoteParticipant(client, rtc, call, room, identity,
                                          userId, QStringLiteral("HERDEV"),
                                          QStringLiteral("$m1")) != nullptr);
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false),
                             sfuTrack(QStringLiteral("screen_share_audio"),
                                      QStringLiteral("TR_share_audio"),
                                      false) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 1);
        // Unity and untouched, so a later record can only come from the signal.
        QCOMPARE(call.engineShareVolumeForTest(QStringLiteral("TR_share_a")),
                 -1);

        // The change comes from settings; no participant volume is touched.
        settings.setCallShareVolume(userId, 60);
        QVERIFY2(call.engineShareVolumeForTest(QStringLiteral("TR_share_a"))
                     != -1,
                 "callShareVolumeChanged has no consumer, so a share level "
                 "changed elsewhere never reaches a live call");
        QCOMPARE(call.engineShareVolumeForTest(QStringLiteral("TR_share_a")),
                 60);
    }

    // A stored share level reaches the engine when the share's audio track
    // (distinct from the sharer's microphone) is listed after the share row.
    void aStoredShareVolumeReachesTheEngineWhenItsAudioTrackArrivesLate()
    {
        const QString room = QStringLiteral("!shlate:example.org");
        const QString identity = QStringLiteral("@her:example.org:HERDEV");
        const QString userId = QStringLiteral("@her:example.org");

        SettingsManager settings;
        settings.saveSession(QStringLiteral("https://example.org"),
                             QStringLiteral("@me:example.org"),
                             QStringLiteral("MEDEV"),
                             QStringLiteral("token-fixture"));
        settings.setCallShareVolume(userId, 100);
        QCOMPARE(settings.callShareVolume(userId), 100);

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        call.setSettings(&settings);
        QVERIFY(stageOneRemoteParticipant(client, rtc, call, room, identity,
                                          userId, QStringLiteral("HERDEV"),
                                          QStringLiteral("$m1")) != nullptr);

        // A silent share first: the row exists, its audio track does not.
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 1);
        call.setShareVolume(QStringLiteral("TR_share_a"), 35);
        QCOMPARE(call.engineShareVolumeForTest(QStringLiteral("TR_share_a")),
                 -1);

        // ...and now the share's audio is unmuted.
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false),
                             sfuTrack(QStringLiteral("screen_share_audio"),
                                      QStringLiteral("TR_share_audio"),
                                      false) }),
        });
        QVERIFY2(call.engineShareVolumeForTest(QStringLiteral("TR_share_a"))
                     != -1,
                 "the share level never reached the audio graph once its "
                 "audio track appeared; shareVolume() would still say 35");
        QCOMPARE(call.engineShareVolumeForTest(QStringLiteral("TR_share_a")),
                 35);
    }

    void aShareVolumeIsRememberedUnderItsOwner()
    {
        const QString room = QStringLiteral("!share:example.org");
        const QString identity = QStringLiteral("@her:example.org:HERDEV");
        const QString userId = QStringLiteral("@her:example.org");

        SettingsManager settings;
        settings.saveSession(QStringLiteral("https://example.org"),
                             QStringLiteral("@me:example.org"),
                             QStringLiteral("MEDEV"),
                             QStringLiteral("token-fixture"));

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        call.setSettings(&settings);
        QVERIFY(stageOneRemoteParticipant(client, rtc, call, room, identity,
                                          userId, QStringLiteral("HERDEV"),
                                          QStringLiteral("$m1")) != nullptr);

        // A share's volume is stored under its owner, not the share id, so it
        // survives a restart and a re-share. The store outlives the process:
        // reset to neutral (which removes the key) and prove it clean.
        settings.setCallShareVolume(userId, 100);
        QCOMPARE(settings.callShareVolume(userId), 100);

        // The share id is the track sid, so a restart gives a different one.
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 1);
        call.setShareVolume(QStringLiteral("TR_share_a"), 35);
        QCOMPARE(call.shareVolume(QStringLiteral("TR_share_a")), 35);
        QCOMPARE(settings.callShareVolume(userId), 35);

        // The same person shares again under a new id; only a store keyed by
        // the owner can help. Asserted on the applier's signal, not the
        // getter: shareVolume() falls back to the store and would pass anyway.
        QSignalSpy applied(&call, &SfuCallController::shareVolumeChanged);
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_b"), false) }),
        });
        QVERIFY2(!applied.isEmpty(),
                 "applyStoredShareVolumes() never ran for the new share, so "
                 "the engine was never told and only the getter's store "
                 "fallback would make this look right");
        QCOMPARE(applied.constFirst().at(0).toString(),
                 QStringLiteral("TR_share_b"));
        QCOMPARE(applied.constFirst().at(1).toInt(), 35);
        QCOMPARE(call.shareVolume(QStringLiteral("TR_share_b")), 35);
    }

    void aParticipantVolumeCanBeAmplifiedPastUnity()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("me"));
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("microphone"),
                                      QStringLiteral("TR_1"), false) }),
        });
        const int row = participantRowFor(call.participantModel(),
                                          QStringLiteral("alice"));
        QVERIFY(row >= 0);

        call.setParticipantVolume(QStringLiteral("alice"), 200);
        QCOMPARE(participantRole(call.participantModel(), row,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);

        // Still a closed range for hand-edited or runaway values.
        call.setParticipantVolume(QStringLiteral("alice"), 5000);
        QCOMPARE(participantRole(call.participantModel(), row,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);
        call.setParticipantVolume(QStringLiteral("alice"), -40);
        QCOMPARE(participantRole(call.participantModel(), row,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 0);
    }

    // With no settings seam, a volume still works for the call and is simply
    // not remembered.
    void aParticipantVolumeReadsUnityWithNowhereToStoreIt()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        QCOMPARE(call.participantVolume(QStringLiteral("alice")), 100);
    }

    // Camera route: a pure predicate with every input passed in, so each
    // clause is checkable without the hardware (and without
    // HAVE_LIGHTNING_WEBRTC). Proves the decision only, not that either route
    // carries a picture.

    // A sandboxed build always takes the camera portal: Flatpak has no
    // camera-only device permission and Flathub rejects `--device=all`. A
    // portal refusal is something the user can act on; a failed open is not.
    void aSandboxedBuildAlwaysTakesTheCameraPortal()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        for (bool portalUsable : { true, false }) {
            for (bool deviceNode : { true, false }) {
                QCOMPARE(SfuCallController::linuxCameraRoute(
                             /*sandboxed=*/true, portalUsable, deviceNode),
                         Route::Portal);
            }
        }
    }

    // A visible device node keeps `v4l2src`, the live-validated path; the
    // portal must not take over where nothing is wrong.
    void aVisibleDeviceNodeKeepsTheDirectCamera()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        QCOMPARE(SfuCallController::linuxCameraRoute(
                     /*sandboxed=*/false, /*portalUsable=*/true,
                     /*directDeviceVisible=*/true),
                 Route::Direct);
        QCOMPARE(SfuCallController::linuxCameraRoute(
                     /*sandboxed=*/false, /*portalUsable=*/false,
                     /*directDeviceVisible=*/true),
                 Route::Direct);
    }

    // With no device node the portal is used only if usable; otherwise the
    // answer stays Direct (today's behaviour), not a new refusal state.
    void withNoDeviceNodeThePortalIsUsedOnlyWhenItIsUsable()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        QCOMPARE(SfuCallController::linuxCameraRoute(
                     /*sandboxed=*/false, /*portalUsable=*/true,
                     /*directDeviceVisible=*/false),
                 Route::Portal);
        QCOMPARE(SfuCallController::linuxCameraRoute(
                     /*sandboxed=*/false, /*portalUsable=*/false,
                     /*directDeviceVisible=*/false),
                 Route::Direct);
    }

    // The chosen camera route is logged with what decided it, so a silent
    // failure is distinguishable from "portal never asked" or "portal refused".
    void theCameraRouteIsAnnouncedWithWhatDecidedIt()
    {
        QFile file(QStringLiteral(SOURCE_DIR
                                  "/src/calls/SfuCallController.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray source = file.readAll();
        const int at = source.indexOf("\"camera route=\"");
        QVERIFY2(at > 0, "nothing announces which camera route was taken, so "
                         "a silent fallback is unreadable from a log");
        const QByteArray line = source.mid(at, 400);
        for (const char *input : { "sandboxed=", "device_node=",
                                   "portal_wired=", "portal_usable=" }) {
            QVERIFY2(line.contains(input),
                     qPrintable(QStringLiteral("the camera route line does "
                                               "not report %1")
                                    .arg(QLatin1String(input))));
        }
        // The engine separately logs which source it built; a capture can fail
        // between the decision and the pipeline.
        QFile engine(QStringLiteral(SOURCE_DIR
                                    "/src/calls/SfuMediaEngine.cpp"));
        QVERIFY(engine.open(QIODevice::ReadOnly));
        QVERIFY2(engine.readAll().contains("\"camera source=\""),
                 "the engine does not report which camera source it built");
    }

    void theDesktopPortalIsPreferredOverTheFallbackInEverySession()
    {
        using Route = SfuCallController::LinuxShareRoute;
        const auto route = [](const QString &platform, const QString &session,
                              const QString &wayland, const QString &x11,
                              bool element) {
            return SfuCallController::linuxShareRoute(
                /*portalAvailable=*/true, platform, session, wayland, x11,
                element);
        };
        QCOMPARE(route(QStringLiteral("wayland"), QStringLiteral("wayland"),
                       QStringLiteral("wayland-0"), QString(), false),
                 Route::Portal);
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("x11"), QString(),
                       QStringLiteral(":0"), true),
                 Route::Portal);
        // Linux share routing is one pure function, testable without a display
        // server, portal or GStreamer. The portal always wins when available:
        // it is what makes Wayland sharing safe. Even with nothing else
        // working, a reachable portal is the answer.
        QCOMPARE(route(QString(), QString(), QString(), QString(), false),
                 Route::Portal);
        // A portal route shows the user nothing; nothing went wrong.
        QVERIFY(SfuCallController::linuxShareRefusal(Route::Portal).isEmpty());
    }

    // Wayland with no portal refuses with a reason rather than offering the
    // picker: XWayland provides a working DISPLAY, but its root window
    // captures as black. Each Wayland signal must suffice on its own, with an
    // X11 display and the capture element present.
    void aWaylandSessionWithNoPortalRefusesInsteadOfOfferingAPickerItCannotHonour()
    {
        using Route = SfuCallController::LinuxShareRoute;
        const auto route = [](const QString &platform, const QString &session,
                              const QString &wayland) {
            return SfuCallController::linuxShareRoute(
                /*portalAvailable=*/false, platform, session, wayland,
                /*x11Display=*/QStringLiteral(":0"),
                /*captureElementPresent=*/true);
        };
        // 1. The platform plugin says so.
        QCOMPARE(route(QStringLiteral("wayland"), QString(), QString()),
                 Route::RefuseWaylandNeedsPortal);
        QCOMPARE(route(QStringLiteral("wayland-egl"), QString(), QString()),
                 Route::RefuseWaylandNeedsPortal);
        // 2. The session type says so while Qt is on xcb (an XWayland Qt app).
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("wayland"),
                       QString()),
                 Route::RefuseWaylandNeedsPortal);
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("Wayland"),
                       QString()),
                 Route::RefuseWaylandNeedsPortal);
        // 3. Only `WAYLAND_DISPLAY` says so.
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("x11"),
                       QStringLiteral("wayland-0")),
                 Route::RefuseWaylandNeedsPortal);

        // The message names the cause and an action.
        const QString message = SfuCallController::linuxShareRefusal(
            Route::RefuseWaylandNeedsPortal);
        QVERIFY(!message.isEmpty());
        QVERIFY2(message.contains(QStringLiteral("xdg-desktop-portal")),
                 qPrintable(QStringLiteral("the Wayland refusal does not name "
                                           "the thing that is missing: %1")
                                .arg(message)));
        QVERIFY2(message != QStringLiteral(
                     "Screen sharing isn't available on this desktop."),
                 "the Wayland refusal is still the old unactionable sentence");
    }

    // An X11 session with no portal falls back to Lightning's own picker.
    void anX11SessionWithNoPortalFallsBackToLightningsOwnPicker()
    {
        using Route = SfuCallController::LinuxShareRoute;
        QCOMPARE(SfuCallController::linuxShareRoute(
                     /*portalAvailable=*/false, QStringLiteral("xcb"),
                     QStringLiteral("x11"), /*waylandDisplay=*/QString(),
                     QStringLiteral(":0"), /*captureElementPresent=*/true),
                 Route::FallbackDisplays);
        // An unset session type must not decide.
        QCOMPARE(SfuCallController::linuxShareRoute(
                     false, QStringLiteral("xcb"), QString(), QString(),
                     QStringLiteral(":1"), true),
                 Route::FallbackDisplays);
        // Offering a picker is not an error, so it says nothing.
        QVERIFY(SfuCallController::linuxShareRefusal(Route::FallbackDisplays)
                    .isEmpty());
    }

    // A missing capture element is refused before the picker is offered,
    // not discovered at PLAYING after the user chose.
    void anX11SessionWithoutTheCaptureElementRefusesBeforeOfferingAPicker()
    {
        using Route = SfuCallController::LinuxShareRoute;
        QCOMPARE(SfuCallController::linuxShareRoute(
                     false, QStringLiteral("xcb"), QStringLiteral("x11"),
                     QString(), QStringLiteral(":0"),
                     /*captureElementPresent=*/false),
                 Route::RefuseNoCaptureElement);
        const QString message = SfuCallController::linuxShareRefusal(
            Route::RefuseNoCaptureElement);
        QVERIFY2(message.contains(QStringLiteral("ximagesrc")),
                 qPrintable(QStringLiteral("the refusal does not name the "
                                           "missing element: %1")
                                .arg(message)));

        // In a sandbox the host's packages do not help: the Flatpak loads
        // plugins from its runtime and /app only, and the KDE runtime has no
        // ximagesrc. The advice must not point at apt.
        const QString sandboxed = SfuCallController::linuxShareRefusal(
            Route::RefuseNoCaptureElement, /*sandboxed=*/true);
        QVERIFY2(!sandboxed.contains(QStringLiteral("gst-plugins-good")),
                 qPrintable(QStringLiteral("a sandboxed build is told to "
                                           "install a host package: %1")
                                .arg(sandboxed)));
        QVERIFY2(sandboxed.contains(QStringLiteral("xdg-desktop-portal")),
                 qPrintable(QStringLiteral("the sandboxed refusal does not "
                                           "name the portal: %1")
                                .arg(sandboxed)));
        // Nor is the portal the usual remedy: this route is X11-only, where
        // few portal backends offer ScreenCast. Name what works instead: a
        // build that captures X11 itself, or a Wayland session.
        QVERIFY2(sandboxed.contains(QStringLiteral("AppImage"))
                     && sandboxed.contains(QStringLiteral(
                         "distribution package")),
                 qPrintable(QStringLiteral("the sandboxed refusal no longer "
                                           "names the builds that capture "
                                           "X11 directly: %1")
                                .arg(sandboxed)));
        QVERIFY2(sandboxed.contains(QStringLiteral("Wayland session")),
                 qPrintable(QStringLiteral("the sandboxed refusal no longer "
                                           "names the Wayland route: %1")
                                .arg(sandboxed)));
        QVERIFY2(!sandboxed.contains(QStringLiteral("xdg-desktop-portal-gnome")),
                 qPrintable(QStringLiteral("the sandboxed refusal names one "
                                           "desktop's portal package: %1")
                                .arg(sandboxed)));
        // An unsandboxed build keeps the package advice.
        QVERIFY(message.contains(QStringLiteral("gst-plugins-good")));
        // The other refusals do not depend on the sandbox.
        QCOMPARE(SfuCallController::linuxShareRefusal(
                     Route::RefuseWaylandNeedsPortal, true),
                 SfuCallController::linuxShareRefusal(
                     Route::RefuseWaylandNeedsPortal, false));

        // No display server outranks the element probe: nothing to capture.
        QCOMPARE(SfuCallController::linuxShareRoute(false, QStringLiteral("xcb"),
                                                    QString(), QString(),
                                                    /*x11Display=*/QString(),
                                                    true),
                 Route::RefuseNoDisplayServer);
        QVERIFY(!SfuCallController::linuxShareRefusal(
                     Route::RefuseNoDisplayServer)
                     .isEmpty());
    }

    // The X11 capture rectangle must come from the platform, never be derived
    // from QScreen::geometry() * devicePixelRatio(): Qt keeps the top-left in
    // native pixels and scales only the size, and devicePixelRatio() is a
    // rounded value, so the derivation lands inside a neighbouring monitor.
    // Scans these two function bodies (the Windows branch legitimately uses
    // devicePixelRatio() for a label); each slice is proven before use.
    void theLinuxCaptureRectangleIsNeverDerivedFromDevicePixelRatio()
    {
        QFile file(QStringLiteral(
            SOURCE_DIR "/src/calls/SfuCallController.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "SfuCallController.cpp is not where this test looks for it");
        const QString source = QString::fromUtf8(file.readAll());

        // Line comments stripped first: the bodies explain why
        // devicePixelRatio() is wrong, which would trip the ban. There are no
        // block comments here.
        const auto stripComments = [](const QString &in) {
            QString out;
            out.reserve(in.size());
            for (const QString &line : in.split(QLatin1Char('\n'))) {
                const int at = line.indexOf(QStringLiteral("//"));
                out += (at >= 0 ? line.left(at) : line);
                out += QLatin1Char('\n');
            }
            return out;
        };
        const auto slice = [&source, &stripComments](const QString &from,
                                                     const QString &to) {
            const int a = source.indexOf(from);
            const int b = a >= 0 ? source.indexOf(to, a + from.size()) : -1;
            return (a >= 0 && b > a) ? stripComments(source.mid(a, b - a))
                                     : QString();
        };

        const QString nativeRect =
            slice(QStringLiteral("QRect SfuCallController::nativeScreenRect"),
                  QStringLiteral("SfuCallController::physicalRectForScreenNamed"));
        QVERIFY2(!nativeRect.isEmpty(),
                 "could not slice nativeScreenRect — this scan would pass "
                 "without reading anything");
        QVERIFY2(nativeRect.contains(QStringLiteral("handle()")),
                 "the nativeScreenRect slice does not contain the code it is "
                 "meant to be scanning");
        QVERIFY2(!nativeRect.contains(QStringLiteral("devicePixelRatio")),
                 "nativeScreenRect derives the rectangle from "
                 "devicePixelRatio() again — Qt does not scale the origin and "
                 "the ratio is rounded, so this captures the wrong display");

        const QString populate = slice(
            QStringLiteral("bool SfuCallController::populateLinuxDisplaySources"),
            QStringLiteral("void SfuCallController::requestScreenShare"));
        QVERIFY2(!populate.isEmpty(),
                 "could not slice populateLinuxDisplaySources");
        QVERIFY2(populate.contains(QStringLiteral("nativeScreenRect")),
                 "the populateLinuxDisplaySources slice does not contain the "
                 "code it is meant to be scanning");
        QVERIFY2(!populate.contains(QStringLiteral("devicePixelRatio")),
                 "populateLinuxDisplaySources scales a screen rectangle by "
                 "devicePixelRatio() again");
    }

    void aCaptureRectangleIsAcceptedAsTheNativeRectangleOrNotAtAll()
    {
        // The validator accepts native rectangles unchanged and refuses what
        // `ximagesrc` cannot be given. These real rectangles pass unchanged.
        QCOMPARE(SfuCallController::validX11CaptureRect(
                     QRect(0, 0, 3840, 2160)),
                 QRect(0, 0, 3840, 2160));
        QCOMPARE(SfuCallController::validX11CaptureRect(
                     QRect(3840, 0, 3840, 2160)),
                 QRect(3840, 0, 3840, 2160));
        // A vertically stacked second monitor, same rule.
        QCOMPARE(SfuCallController::validX11CaptureRect(
                     QRect(0, 2160, 1920, 1080)),
                 QRect(0, 2160, 1920, 1080));

        // Refused rather than clamped: `ximagesrc`'s coordinates are unsigned,
        // so a negative origin wraps and captures somewhere else.
        QVERIFY(!SfuCallController::validX11CaptureRect(
                     QRect(-1920, 0, 1920, 1080))
                     .isValid());
        QVERIFY(!SfuCallController::validX11CaptureRect(
                     QRect(0, -100, 800, 600))
                     .isValid());
        // Degenerate input produces no rectangle rather than an invented one.
        QVERIFY(!SfuCallController::validX11CaptureRect(QRect(0, 0, 0, 0))
                     .isValid());
        QVERIFY(!SfuCallController::validX11CaptureRect(QRect()).isValid());

        // A null screen handle is not a rectangle; QScreen::handle() is null
        // during hot-unplug teardown.
        QVERIFY(!SfuCallController::nativeScreenRect(nullptr).isValid());
    }

    // The peer is bound: (room, call_id) identifies the call, but call_id is
    // readable by every room member, so it does not say who speaks for the
    // other side.

    void aHangupFromAnotherRoomMemberDoesNotEndTheRing()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        QSignalSpy endedSpy(&calls, &CallController::incomingCallEnded);
        CallSignal hangup;
        hangup.kind = CallSignal::Kind::Hangup;
        hangup.roomId = QStringLiteral("!r:x");
        hangup.eventId = QStringLiteral("$hangup-stranger");
        hangup.sender = QStringLiteral("@stranger:x"); // not the caller
        hangup.callId = QStringLiteral("call-1");
        hangup.partyId = QStringLiteral("stranger-party");
        hangup.reason = QStringLiteral("user_hangup");
        client.emitSignal(hangup);
        // Still ringing, and no "missed call" was synthesised from someone
        // who never called.
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(endedSpy.count(), 0);

        // A select_answer from the stranger cannot silence it either.
        CallSignal selected = hangup;
        selected.kind = CallSignal::Kind::SelectAnswer;
        selected.eventId = QStringLiteral("$select-stranger");
        selected.selectedPartyId = QStringLiteral("somebody-else");
        client.emitSignal(selected);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        // The caller's own hangup still ends it.
        hangup.sender = QStringLiteral("@peer:x");
        hangup.partyId = QStringLiteral("peer-party");
        hangup.eventId = QStringLiteral("$hangup-peer");
        client.emitSignal(hangup);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::RemoteHangup);
    }

    void anAnswerFromSomeoneOtherThanTheInviteeIsIgnored()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                         QStringLiteral("v=0 sdp"), 60000,
                                         QStringLiteral("@peer:x")));
        const QString callId = calls.activeCallId();

        // A third member answers first; they must not win the media session.
        CallSignal intruder;
        intruder.kind = CallSignal::Kind::Answer;
        intruder.roomId = QStringLiteral("!r:x");
        intruder.eventId = QStringLiteral("$answer-intruder");
        intruder.sender = QStringLiteral("@intruder:x");
        intruder.callId = callId;
        intruder.partyId = QStringLiteral("intruder-party");
        client.emitSignal(intruder);
        QCOMPARE(calls.state(), CallController::State::Inviting);
        for (const auto &event : client.sent)
            QVERIFY(event.kind != QLatin1String("select_answer"));

        CallSignal answer = intruder;
        answer.eventId = QStringLiteral("$answer-peer");
        answer.sender = QStringLiteral("@peer:x");
        answer.partyId = QStringLiteral("peer-party");
        client.emitSignal(answer);
        QCOMPARE(calls.state(), CallController::State::Connecting);
        int selectAnswers = 0;
        for (const auto &event : client.sent) {
            if (event.kind == QLatin1String("select_answer")) {
                ++selectAnswers;
                QCOMPARE(event.extra, QStringLiteral("peer-party"));
            }
        }
        QCOMPARE(selectAnswers, 1);
    }

    void withNoInviteeTheFirstAnswerLocksThePeer()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                         QStringLiteral("v=0 sdp")));
        const QString callId = calls.activeCallId();
        CallSignal answer;
        answer.kind = CallSignal::Kind::Answer;
        answer.roomId = QStringLiteral("!r:x");
        answer.eventId = QStringLiteral("$answer-1");
        answer.sender = QStringLiteral("@peer:x");
        answer.callId = callId;
        answer.partyId = QStringLiteral("peer-party");
        client.emitSignal(answer);
        QCOMPARE(calls.state(), CallController::State::Connecting);

        // Once locked, a hangup from anyone else is not this call's.
        CallSignal hangup;
        hangup.kind = CallSignal::Kind::Hangup;
        hangup.roomId = QStringLiteral("!r:x");
        hangup.eventId = QStringLiteral("$hangup-other");
        hangup.sender = QStringLiteral("@other:x");
        hangup.callId = callId;
        hangup.partyId = QStringLiteral("other-party");
        hangup.reason = QStringLiteral("user_hangup");
        client.emitSignal(hangup);
        QCOMPARE(calls.state(), CallController::State::Connecting);
        hangup.sender = QStringLiteral("@peer:x");
        hangup.partyId = QStringLiteral("peer-party");
        client.emitSignal(hangup);
        QCOMPARE(calls.state(), CallController::State::Ended);
    }


    // Failure reporting for group call joins.

    // Every category Rust can emit has its own wording: nothing may reach the
    // generic fallback, categories grouped on purpose share exactly one
    // sentence, and different groups never collide.
    void everySfuFailureCategoryHasItsOwnHonestWording()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        auto sentenceFor = [&](const QString &category) {
            call.setCallStateForTest(SfuCallController::State::Connected);
            client.emitSfuState(QStringLiteral("failed"), category);
            return call.lastError();
        };

        // The fallback itself; anything equal to it has no wording.
        const QString generic =
            sentenceFor(QStringLiteral("a_category_no_path_emits"));
        QVERIFY(!generic.isEmpty());

        // category -> group whose wording it shares. The log keeps the category
        // verbatim; different groups must read differently.
        const QList<QPair<QString, QString>> table = {
            { QStringLiteral("forbidden"),         QStringLiteral("refused") },
            { QStringLiteral("unsupported"),       QStringLiteral("no-service") },
            { QStringLiteral("unrecognized"),      QStringLiteral("homeserver") },
            { QStringLiteral("not_found"),         QStringLiteral("homeserver") },
            { QStringLiteral("rate_limited"),      QStringLiteral("rate") },
            { QStringLiteral("focus_unroutable"),  QStringLiteral("private") },
            { QStringLiteral("network"),           QStringLiteral("connect") },
            { QStringLiteral("connect_failed"),    QStringLiteral("connect") },
            { QStringLiteral("connection_lost"),   QStringLiteral("connect") },
            { QStringLiteral("send_failed"),       QStringLiteral("connect") },
            { QStringLiteral("transport_failed"),  QStringLiteral("connect") },
            { QStringLiteral("server_error"),      QStringLiteral("trouble") },
            { QStringLiteral("invalid"),           QStringLiteral("misconfigured") },
            { QStringLiteral("invalid_transport"), QStringLiteral("misconfigured") },
            { QStringLiteral("invalid_request"),   QStringLiteral("misconfigured") },
            { QStringLiteral("focus_url_invalid"), QStringLiteral("misconfigured") },
            { QStringLiteral("ws_frame_too_large"),
                                                   QStringLiteral("misconfigured") },
            { QStringLiteral("unknown"),           QStringLiteral("misconfigured") },
            // The former `connect_failed`, split by failure point (Rust
            // `classify_ws_error`): DNS, TLS, HTTP status, websocket upgrade,
            // firewall, dead SFU.
            { QStringLiteral("focus_unresolved"),  QStringLiteral("dns") },
            { QStringLiteral("focus_resolve_timeout"),
                                                   QStringLiteral("dns") },
            { QStringLiteral("focus_private_name"),
                                                   QStringLiteral("local-name") },
            { QStringLiteral("sfu_unreachable"),   QStringLiteral("no-route") },
            { QStringLiteral("sfu_refused_connection"),
                                                   QStringLiteral("refused-port") },
            { QStringLiteral("connect_blocked"),   QStringLiteral("blocked") },
            { QStringLiteral("connect_timeout"),   QStringLiteral("timeout") },
            { QStringLiteral("tls_failed"),        QStringLiteral("tls") },
            { QStringLiteral("ws_rejected"),       QStringLiteral("not-a-ws") },
            { QStringLiteral("ws_handshake_failed"),
                                                   QStringLiteral("not-a-ws") },
            // Websocket-upgrade twins of the JWT service categories; same fact
            // one step later, same sentence.
            { QStringLiteral("sfu_forbidden"),     QStringLiteral("refused") },
            { QStringLiteral("sfu_not_found"),     QStringLiteral("no-service") },
        };

        QHash<QString, QString> saidForGroup;
        for (const auto &row : table) {
            const QString said = sentenceFor(row.first);
            QVERIFY2(!said.isEmpty(),
                     qPrintable(QStringLiteral("`%1` produced no message at "
                                               "all").arg(row.first)));
            QVERIFY2(said != generic,
                     qPrintable(QStringLiteral(
                                    "`%1` still falls through to the generic "
                                    "sentence, so the user is told \"%2\" for "
                                    "a failure that has a real explanation")
                                    .arg(row.first, generic)));
            const auto known = saidForGroup.constFind(row.second);
            if (known == saidForGroup.cend()) {
                saidForGroup.insert(row.second, said);
                continue;
            }
            QVERIFY2(*known == said,
                     qPrintable(QStringLiteral(
                                    "`%1` was meant to share the `%2` wording "
                                    "and says something else: \"%3\" vs "
                                    "\"%4\"").arg(row.first, row.second, said,
                                                  *known)));
        }

        // Distinct groups must be distinct sentences.
        QStringList distinct = saidForGroup.values();
        distinct.sort();
        QStringList deduped = distinct;
        deduped.removeDuplicates();
        QVERIFY2(distinct == deduped,
                 "two failure groups produce the same sentence, so the user "
                 "still cannot tell them apart");

        // `unsupported` must not blame the user's homeserver: it is a 404 from
        // the SFU's JWT service, named by the oldest membership and often on
        // someone else's infrastructure.
        const QString serviceAbsent = saidForGroup.value(
            QStringLiteral("no-service"));
        QVERIFY2(serviceAbsent.contains(QStringLiteral("calling service"),
                                        Qt::CaseInsensitive),
                 qPrintable(QStringLiteral(
                                "a 404 from the call service must name the "
                                "CALL SERVICE; it said: %1")
                                .arg(serviceAbsent)));
        QVERIFY2(serviceAbsent != saidForGroup.value(
                     QStringLiteral("homeserver")),
                 "the call service having no /sfu/get and the homeserver "
                 "having no calling support are different facts about "
                 "different machines and must not share a sentence");

        // A LAN-only focus must not read as a network outage: Element has no
        // such policy, so the room works there.
        QVERIFY2(saidForGroup.value(QStringLiteral("private"))
                     != saidForGroup.value(QStringLiteral("connect")),
                 "a focus refused for having a private address still reads "
                 "as 'the network is down'");
    }

    // The membership refusal must not point at the permissions screen:
    // `org.matrix.msc3401.call.member` is deliberately absent from
    // RoomInfoController::powerLevelKeys() and the Rust write allowlist.
    void theMembershipRefusalDoesNotPromiseAPermissionsScreenThatCannotHelp()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        const quint64 publish = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        QVERIFY(publish != 0);
        client.refusePublish(publish, QStringLiteral("forbidden"));

        const QString said = call.lastError();
        QVERIFY(!said.isEmpty());
        // Still a room-permission problem; the case separating it from the call
        // service's refusal depends on these two words.
        QVERIFY(said.contains(QStringLiteral("permission")));
        QVERIFY(said.contains(QStringLiteral("room")));
        QVERIFY2(!said.contains(QStringLiteral("in the room's permissions")),
                 qPrintable(QStringLiteral(
                                "the refusal still points at a permissions "
                                "screen that cannot set call membership; it "
                                "said: %1").arg(said)));
    }

    // Leaving during `Preparing`.

    // A publish that lands after we left is retracted with the delay id it
    // armed; otherwise the server may apply it after our retraction and
    // recreate a ghost membership, and the delayed retraction is never
    // cancelled.
    void aMembershipPublishThatLandsAfterWeLeftIsRetracted()
    {
        const QString room = QStringLiteral("!room:example.org");
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        const quint64 publish = call.beginMembershipPublishForTest(
            room, QStringLiteral("https://sfu.example.org"));
        QVERIFY(publish != 0);
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Preparing));

        // Leave while the homeserver is still deciding.
        call.leave();
        QVERIFY2(client.retractions.isEmpty(),
                 "a retraction was sent for a membership that does not exist "
                 "yet — and when the server refuses it, the give-up branch "
                 "reports a ghost membership nobody ever created");

        // ...then the publish lands.
        client.answerPublish(publish, true, QStringLiteral("delay-1"));

        QCOMPARE(client.retractions.size(), 1);
        QCOMPARE(client.retractions.first().first, room);
        QVERIFY2(client.retractions.first().second
                     == QStringLiteral("delay-1"),
                 "the retraction did not carry the delay id THIS publish "
                 "armed, so the server's delayed retraction is left running "
                 "against an id nothing holds");
    }

    // A refused publish leaves nothing to retract, so no retraction (and no
    // false ghost-membership warning) follows.
    void aRefusedPublishIsNotFollowedByARetractionOfNothing()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        const quint64 publish = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        client.refusePublish(publish, QStringLiteral("forbidden"));

        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Failed));
        QVERIFY2(client.retractions.isEmpty(),
                 "a membership the homeserver refused was 'retracted' anyway");
    }

    // A stale publish must not retract the call we are back in: the state key
    // is per (user, device), so both publishes address the same event.
    void aStalePublishDoesNotRetractTheCallWeAreBackIn()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString focus = QStringLiteral("https://sfu.example.org");
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        const quint64 first = call.beginMembershipPublishForTest(room, focus);
        call.leave();
        // Straight back into the same room's call.
        const quint64 second = call.beginMembershipPublishForTest(room, focus);
        QVERIFY(second != first);
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Preparing));

        // The abandoned publish finally lands.
        client.answerPublish(first, true, QStringLiteral("delay-1"));

        QVERIFY2(client.retractions.isEmpty(),
                 "the stale publish's answer retracted the state event the "
                 "call we are in right now owns");
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Preparing));
    }

    // A hand raised before its membership arrived.

    // The reaction rides sync and the membership rides a session read, with
    // no ordering, so an early raise is parked and applied when the
    // membership lands.
    void aRaiseThatBeatsItsMembershipIsAppliedWhenItArrives()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_BEA"), {}),
        });
        CallParticipantModel *model = call.participantModel();
        const int row = participantRowFor(model, identity);
        QVERIFY(row >= 0);

        // The reaction arrives first and cannot be attributed yet.
        client.emitHandChanged(room, QStringLiteral("@bea:example.org"),
                               membership, QStringLiteral("$raise"), true);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 false);

        // ...then the membership lands.
        RtcParticipant bea;
        bea.userId = QStringLiteral("@bea:example.org");
        bea.deviceId = QStringLiteral("BDEV");
        bea.rtcIdentity = identity;
        bea.intent = QStringLiteral("audio");
        bea.membershipEventId = membership;
        bea.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = room;
        session.participants = { bea };
        rtc.refresh(room);
        QVERIFY(!client.sessionReads.isEmpty());
        client.answerSession(client.lastSessionOp, session);

        QVERIFY2(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 "a hand raised before its membership was read stayed down "
                 "for the rest of the call");

        // It can still be lowered: the redaction names only the reaction, so
        // the applied raise must be recorded under that id.
        client.emitHandChanged(room, QString(), QString(),
                               QStringLiteral("$raise"), false);
        QCOMPARE(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 false);
    }

    // A forged raise (sender does not own the annotated membership) is never
    // applied and never parked; forgeries would fill the bounded store. The
    // first half is a guard on the parking code rather than a regression test.
    void aForgedRaiseIsNeitherAppliedNorParked()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_BEA"), {}),
        });
        CallParticipantModel *model = call.participantModel();

        RtcParticipant bea;
        bea.userId = QStringLiteral("@bea:example.org");
        bea.deviceId = QStringLiteral("BDEV");
        bea.rtcIdentity = identity;
        bea.intent = QStringLiteral("audio");
        bea.membershipEventId = membership;
        bea.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = room;
        session.participants = { bea };
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);

        // Mallory raises Bea's hand.
        client.emitHandChanged(room, QStringLiteral("@mallory:example.org"),
                               membership, QStringLiteral("$forged"), true);
        QCOMPARE(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 false);

        // A later membership read must not let it through either.
        RtcParticipant moved = bea;
        moved.displayName = QStringLiteral("Bea");
        RtcSessionData again;
        again.roomId = room;
        again.participants = { moved };
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, again);
        QVERIFY2(!participantRole(model, participantRowFor(model, identity),
                                  CallParticipantModel::HandRaisedRole)
                      .toBool(),
                 "a raise from somebody who does not own the membership it "
                 "annotates was applied on the next session read");
    }

    // Transient call reactions (io.element.call.reaction).

    void aCallReactionIsShownOnTheTileOfWhoeverOwnsTheMembership()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        CallParticipantModel *model = stageOneRemoteParticipant(
            client, rtc, call, room, identity,
            QStringLiteral("@bea:example.org"), QStringLiteral("BDEV"),
            membership);

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kThumbsUpEmoji);
        QCOMPARE(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kThumbsUpEmoji);

        // Nobody else's tile moved: a reaction is drawn on the tile of whoever
        // owns the referenced membership.
        const int mine =
            participantRowFor(model, QStringLiteral("@me:example.org:MEDEV"));
        QVERIFY(mine >= 0);
        QVERIFY2(participantRole(model, mine,
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "somebody else's reaction was drawn on the local tile");
    }

    void aCallReactionEndsWithItsOwnWindowAndADuplicateInsideItIsDropped()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        // The real window, made short; the model's timer and expiry run.
        call.setReactionWindowMsForTest(40);
        CallParticipantModel *model = stageOneRemoteParticipant(
            client, rtc, call, room, identity,
            QStringLiteral("@bea:example.org"), QStringLiteral("BDEV"),
            membership);

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kThumbsUpEmoji);
        const int row = participantRowFor(model, identity);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kThumbsUpEmoji);

        // A second one inside the window changes nothing (element-call refuses
        // too), or a sender could hold a permanent badge by re-sending.
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kPartyEmoji);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kThumbsUpEmoji);

        // ...and it expires on its own.
        QTRY_VERIFY(participantRole(model, row,
                                    CallParticipantModel::ReactionEmojiRole)
                        .toString()
                        .isEmpty());

        // After the window the next one is accepted.
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kPartyEmoji);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kPartyEmoji);
    }

    void aForgedCallReactionIsNeitherShownNorParked()
    {
        // A reaction whose sender does not own the referenced membership is
        // dropped and not parked.
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        CallParticipantModel *model = stageOneRemoteParticipant(
            client, rtc, call, room, identity,
            QStringLiteral("@bea:example.org"), QStringLiteral("BDEV"),
            membership);

        client.emitCallReaction(room, QStringLiteral("@mallory:example.org"),
                                membership, kThumbsUpEmoji);
        QVERIFY2(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "a reaction from somebody who does not own the membership it "
                 "references was drawn on that membership's owner");

        // A later session read must not let it through either.
        RtcParticipant moved;
        moved.userId = QStringLiteral("@bea:example.org");
        moved.deviceId = QStringLiteral("BDEV");
        moved.rtcIdentity = identity;
        moved.intent = QStringLiteral("audio");
        moved.membershipEventId = membership;
        moved.wireFormat = QStringLiteral("session");
        moved.displayName = QStringLiteral("Bea");
        RtcSessionData again;
        again.roomId = room;
        again.participants = { moved };
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, again);
        QVERIFY2(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "a forged reaction was applied on the next session read");
    }

    void anUnknownOrMalformedCallReactionChangesNothing()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        CallParticipantModel *model = stageOneRemoteParticipant(
            client, rtc, call, room, identity,
            QStringLiteral("@bea:example.org"), QStringLiteral("BDEV"),
            membership);
        const int row = participantRowFor(model, identity);
        QSignalSpy changes(model, &QAbstractItemModel::dataChanged);

        // No emoji (rust/src/rtc.rs drops these; this pins the second gate).
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, QString());
        // A membership nobody has ever declared.
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                QStringLiteral("$nobodys-membership"),
                                kThumbsUpEmoji);
        // ...and one for a DIFFERENT room, which this call is not in.
        client.emitCallReaction(QStringLiteral("!elsewhere:example.org"),
                                QStringLiteral("@bea:example.org"), membership,
                                kThumbsUpEmoji);

        QVERIFY(participantRole(model, row,
                                CallParticipantModel::ReactionEmojiRole)
                    .toString()
                    .isEmpty());
        QCOMPARE(changes.count(), 0);
    }

    void aCallReactionThatBeatsItsMembershipIsShownWhenItArrives()
    {
        // Same race as the raised hand, and the same bounded store.
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_BEA"), {}),
        });
        CallParticipantModel *model = call.participantModel();

        // The reaction arrives first and cannot be attributed yet.
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kThumbsUpEmoji);
        QVERIFY(participantRole(model, participantRowFor(model, identity),
                                CallParticipantModel::ReactionEmojiRole)
                    .toString()
                    .isEmpty());

        // ...then the membership lands.
        RtcParticipant bea;
        bea.userId = QStringLiteral("@bea:example.org");
        bea.deviceId = QStringLiteral("BDEV");
        bea.rtcIdentity = identity;
        bea.intent = QStringLiteral("audio");
        bea.membershipEventId = membership;
        bea.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = room;
        session.participants = { bea };
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);

        QCOMPARE(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kThumbsUpEmoji);
    }

    void aParkedCallReactionThatOutlivedItsWindowIsNeverDrawn()
    {
        // A reaction parked longer than its window is over and is not drawn
        // when the membership finally arrives (unlike a hand).
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setReactionWindowMsForTest(20);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));
        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_BEA"), {}),
        });
        CallParticipantModel *model = call.participantModel();

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kThumbsUpEmoji);
        QTest::qWait(60);

        RtcParticipant bea;
        bea.userId = QStringLiteral("@bea:example.org");
        bea.deviceId = QStringLiteral("BDEV");
        bea.rtcIdentity = identity;
        bea.intent = QStringLiteral("audio");
        bea.membershipEventId = membership;
        bea.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = room;
        session.participants = { bea };
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);

        QVERIFY2(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "a reaction whose window had already passed was drawn when "
                 "its membership finally arrived");
    }

    void noReactionOutlivesTheParticipantOrTheCall()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString identity = QStringLiteral("@bea:example.org:BDEV");
        const QString membership = QStringLiteral("$bea-membership");

        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        CallParticipantModel *model = stageOneRemoteParticipant(
            client, rtc, call, room, identity,
            QStringLiteral("@bea:example.org"), QStringLiteral("BDEV"),
            membership);

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kThumbsUpEmoji);
        QVERIFY(!participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty());

        // The participant leaves: the row and reaction go and do not return.
        QVariantMap gone =
            sfuParticipant(identity, QStringLiteral("PA_ONE"), {});
        gone.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
        call.ingestParticipantsForTest({ gone });
        QCOMPARE(participantRowFor(model, identity), -1);

        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"), {}),
        });
        QVERIFY2(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "a reaction survived the participant who sent it leaving");

        // The call ends: nothing transient outlives it.
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kPartyEmoji);
        QVERIFY(!participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty());
        call.leave();
        QCOMPARE(model->rowCount(), 0);
    }

    void sendingAReactionReferencesOurOwnMembershipAndIsNeverOptimistic()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString ownIdentity = QStringLiteral("@me:example.org:MEDEV");
        const QString ownMembership = QStringLiteral("$my-membership");

        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:example.org");
        RtcController rtc;
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(ownIdentity);
        call.ingestParticipantsForTest({
            sfuParticipant(ownIdentity, QStringLiteral("PA_ME"), {}),
        });
        CallParticipantModel *model = call.participantModel();

        // Our membership as the session read reports it; preferred over the
        // published id because a refresh replaces the state event.
        RtcParticipant me;
        me.userId = QStringLiteral("@me:example.org");
        me.deviceId = QStringLiteral("MEDEV");
        me.rtcIdentity = ownIdentity;
        me.intent = QStringLiteral("audio");
        me.membershipEventId = ownMembership;
        me.wireFormat = QStringLiteral("session");
        me.ownUser = true;
        me.ownDevice = true;
        RtcSessionData session;
        session.roomId = room;
        session.participants = { me };
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);

        call.sendCallReaction(kThumbsUpEmoji,
                              QStringLiteral("thumbsup"));
        QCOMPARE(client.reactionSends.size(), 1);
        QCOMPARE(client.reactionSends.at(0).roomId, room);
        QCOMPARE(client.reactionSends.at(0).membershipEventId, ownMembership);
        QCOMPARE(client.reactionSends.at(0).emoji,
                 kThumbsUpEmoji);
        QCOMPARE(client.reactionSends.at(0).name,
                 QStringLiteral("thumbsup"));

        // Not optimistic: our tile lights from the event coming back through
        // sync, like anyone else's.
        QVERIFY2(participantRole(model, participantRowFor(model, ownIdentity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "the sender's own tile was lit before the event existed");

        // A second press inside the window sends nothing; receivers would drop
        // it, and a held control must not become an event storm.
        call.sendCallReaction(kPartyEmoji,
                              QStringLiteral("party"));
        QCOMPARE(client.reactionSends.size(), 1);

        // ...unless the send failed, so the user can retry at once.
        client.answerRtcSend(client.lastReactionOp, false,
                             QStringLiteral("network"));
        call.sendCallReaction(kPartyEmoji,
                              QStringLiteral("party"));
        QCOMPARE(client.reactionSends.size(), 2);

        // The returning event draws it, attributed through our membership.
        client.emitCallReaction(room, QStringLiteral("@me:example.org"),
                                ownMembership,
                                kPartyEmoji);
        QCOMPARE(participantRole(model, participantRowFor(model, ownIdentity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kPartyEmoji);
    }

    // A share-audio failure costs the share's sound, not the call:
    // `share_audio_failed` must not reach the teardown in onEngineFailed.
    void aShareAudioFailureCostsTheSoundAndNotTheCall()
    {
        QVERIFY(SfuCallController::categoryIsShareAudioOnly(
            QStringLiteral("share_audio_failed")));
        QVERIFY(SfuCallController::categoryIsShareAudioOnly(
            QStringLiteral("share_audio_unavailable")));
        // Not a catch-all: everything else still ends the call.
        QVERIFY(!SfuCallController::categoryIsShareAudioOnly(
            QStringLiteral("connection_lost")));
        QVERIFY(!SfuCallController::categoryIsShareAudioOnly(
            QStringLiteral("share_audio")));

        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);

        QSignalSpy failures(&call, &SfuCallController::callFailed);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("share_audio_failed"))));

        QCOMPARE(call.state(), SfuCallController::State::Connected);
        QVERIFY2(call.active(),
                 "a share-audio failure ended the call the user was in");
        // The user is still told.
        QCOMPARE(failures.count(), 1);
        const QString sentence = failures.at(0).at(0).toString();
        QVERIFY2(!sentence.isEmpty(), "no wording for share_audio_failed");
        QVERIFY2(!sentence.contains(QStringLiteral("call ended"),
                                    Qt::CaseInsensitive),
                 qPrintable(QStringLiteral(
                     "a share-audio failure tells the user the call ended: %1")
                     .arg(sentence)));

        // A running share holds a cid that must be released. The engine emits
        // `failed()` synchronously, re-entering from inside
        // publishShareAudio(), so startScreenShare records the cid before that
        // call.
        SfuCallController running;
        running.setClient(&client);
        running.setCallStateForTest(SfuCallController::State::Connected);
        running.setShareAudioCidForTest(QStringLiteral("cid-share-audio"));
        QVERIFY(QMetaObject::invokeMethod(
            &running, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("share_audio_failed"))));
        QVERIFY2(running.shareAudioCidForTest().isEmpty(),
                 "the share-audio track id survived its own failure, so the "
                 "track stays declared to the SFU with nothing behind it");
        QVERIFY2(running.active(), "the cleanup path ended the call");

        // And the ordinary categories are untouched.
        SfuCallController fatal;
        fatal.setClient(&client);
        fatal.setCallStateForTest(SfuCallController::State::Connected);
        QVERIFY(QMetaObject::invokeMethod(
            &fatal, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("connection_lost"))));
        QCOMPARE(fatal.state(), SfuCallController::State::Failed);
    }


private:
    QTemporaryDir m_configHome;
};


QTEST_GUILESS_MAIN(CallControllerTest)
#include "CallControllerTest.moc"
