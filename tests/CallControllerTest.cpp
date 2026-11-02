#include <QtTest/QtTest>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QMetaProperty>
#include <QSignalSpy>

#include <QAbstractItemModel>
#include <limits>

#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/CallParticipantModel.h"
#include "calls/CallShareModel.h"
#include "calls/CallStageState.h"
#include "calls/OneToOneDm.h"
#include "calls/CameraPortal.h"
#include "calls/SdpStore.h"
#include "calls/RtcController.h"
#include "calls/ScreenCastPortal.h"
#include <QSettings>
#include <QTemporaryDir>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif
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
    QString extra;
};

class RecordingCallClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsCallSignaling() const override { return true; }
    bool networkRouteActive() const override { return routed; }
    bool routed = false;
    QString currentUserId() const override
    {
        return simulatedUserId.isEmpty() ? MockMatrixClient::currentUserId()
                                         : simulatedUserId;
    }
    QString simulatedUserId;
    QString currentDeviceId() const override { return simulatedDeviceId; }
    QString simulatedDeviceId;
    // Rooms a case describes itself (a DM, say); others are the mock's.
    RoomInfo roomInfo(const QString &roomId) const override
    {
        const auto it = roomOverrides.constFind(roomId);
        return it != roomOverrides.constEnd() ? *it
                                              : MockMatrixClient::roomInfo(roomId);
    }
    QHash<QString, RoomInfo> roomOverrides;

    quint64 callInvite(const QString &roomId, const QString &callId,
                       const QString &partyId, const QString &offerType,
                       const QString &offerSdp, quint64 lifetimeMs,
                       const QString &invitee,
                       const QString &streamMetadataJson) override
    {
        Q_UNUSED(offerType); Q_UNUSED(offerSdp); Q_UNUSED(lifetimeMs);
        Q_UNUSED(invitee);
        sent.append({QStringLiteral("invite"), roomId, callId, partyId, {}});
        lastStreamMetadata = streamMetadataJson;
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
                       const QString &answerSdp,
                       const QString &streamMetadataJson) override
    {
        Q_UNUSED(answerType);
        sent.append({QStringLiteral("answer"), roomId, callId, partyId,
                     answerSdp.isEmpty() ? QString()
                                         : QStringLiteral("<sdp>")});
        lastStreamMetadata = streamMetadataJson;
        return ++opCounter;
    }
    quint64 callNegotiate(const QString &roomId, const QString &callId,
                          const QString &partyId, const QString &type,
                          const QString &sdp, quint64 lifetimeMs,
                          const QString &streamMetadataJson) override
    {
        Q_UNUSED(sdp); Q_UNUSED(lifetimeMs);
        sent.append({QStringLiteral("negotiate"), roomId, callId, partyId,
                     type});
        lastStreamMetadata = streamMetadataJson;
        return ++opCounter;
    }
    quint64 callStreamMetadata(const QString &roomId, const QString &callId,
                               const QString &partyId,
                               const QString &streamMetadataJson) override
    {
        sent.append({QStringLiteral("stream_metadata"), roomId, callId, partyId,
                     {}});
        lastStreamMetadata = streamMetadataJson;
        return ++opCounter;
    }
    QString lastStreamMetadata;
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
        return deferTurn ? lastTurnOp : 0;
    }
    bool deferTurn = false;
    void sfuMuteTrack(const QString &sid, bool muted) override
    {
        muteRequests.append(qMakePair(sid, muted));
    }
    QList<QPair<QString, bool>> muteRequests;

    quint64 rtcPublishMembership(const QString &roomId,
                                 const QString &focusUrl,
                                 const QString &intent,
                                 bool multiSfu = true) override
    {
        Q_UNUSED(focusUrl); Q_UNUSED(intent);
        publishedMultiSfu.append(multiSfu);
        publishes.append(roomId);
        lastPublishOp = ++opCounter;
        return lastPublishOp;
    }
    QList<bool> publishedMultiSfu;
    quint64 rtcRestartDelayedLeave(const QString &delayId) override
    {
        delayedRestarts.append(delayId);
        lastRestartOp = ++opCounter;
        return lastRestartOp;
    }
    bool supportsMatrixRtc() const override { return true; }
    quint64 rtcTransports(const QString &roomId) override
    {
        Q_UNUSED(roomId);
        lastTransportsOp = ++opCounter;
        return lastTransportsOp;
    }
    void answerTransports(const QStringList &urls)
    {
        Q_EMIT rtcTransportsReceived(lastTransportsOp, true, QString(),
                                      urls, QString());
    }
    quint64 lastTransportsOp = 0;
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
    void refusePublish(quint64 opId, const QString &category)
    {
        Q_EMIT rtcMembershipPublished(opId, false, category, QString(),
                                      QString(), QString());
    }
    void emitSfuState(const QString &state, const QString &category)
    {
        Q_EMIT sfuStateChanged(state, category);
    }
    void emitHandChanged(const QString &roomId, const QString &sender,
                         const QString &membershipEventId,
                         const QString &reactionEventId, bool raised)
    {
        Q_EMIT rtcHandChanged(roomId, sender, membershipEventId,
                              reactionEventId, raised);
    }
    struct SentNotify {
        QString roomId;
        QString type;
        QString intent;
        QString membershipEventId;
    };
    QList<SentNotify> notifies;
    quint64 rtcNotify(const QString &roomId, const QString &notificationType,
                      const QString &intent, quint64 lifetimeMs,
                      const QString &membershipEventId) override
    {
        Q_UNUSED(lifetimeMs);
        notifies.append({roomId, notificationType, intent, membershipEventId});
        lastNotifyOp = ++opCounter;
        return lastNotifyOp;
    }
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
    void emitCallReaction(const QString &roomId, const QString &sender,
                          const QString &membershipEventId,
                          const QString &emoji)
    {
        Q_EMIT rtcCallReactionReceived(roomId, sender, membershipEventId,
                                       emoji);
    }
    void answerRtcSend(quint64 opId, bool ok, const QString &category)
    {
        Q_EMIT rtcSendFinished(opId, ok, category, QString());
    }
    quint64 sfuConnect(const QString &serviceUrl,
                       const QString &roomId) override
    {
        sfuConnects.append(qMakePair(serviceUrl, roomId));
        return ++opCounter;
    }
    quint64 sfuConnectMany(const QStringList &focusUrls,
                           const QString &roomId) override
    {
        if (focusUrls.isEmpty())
            return 0;
        focusPlans.append(focusUrls);
        return sfuConnect(focusUrls.first(), roomId);
    }
    QList<QStringList> focusPlans;
    void sfuDisconnect() override { ++sfuDisconnects; }
    int sfuDisconnects = 0;
    QList<QPair<QString, QString>> sfuConnects;
    void emitCandidates(const QString &roomId, const QString &callId,
                        bool own, const QVariantList &candidates,
                        const QString &sender = QStringLiteral("@peer:x"))
    {
        Q_EMIT callCandidatesReceived(roomId, callId,
                                      QStringLiteral("peer-party"),
                                      sender, own,
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
    quint64 lastNotifyOp = 0;
};

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
    bool supportsVideo() const override { return videoEngine; }
    QString localStreamId() const override { return streamId; }
    void prepareCall(const QString &callId, bool video, bool cameraOn) override
    {
        prepared.append(QStringLiteral("%1 video=%2 camera=%3")
                            .arg(callId)
                            .arg(video)
                            .arg(cameraOn));
    }
    void setCameraEnabled(const QString &callId, bool on) override
    {
        cameraRequests.append(on);
        Q_UNUSED(callId);
    }
    void applyNegotiation(const QString &callId, const QString &type,
                          const QString &sdp) override
    {
        Q_UNUSED(callId);
        negotiations.append(type + QLatin1Char(' ') + sdp);
    }
    void deliverCameraFailure(const QString &callId)
    {
        Q_EMIT cameraFailed(callId, QStringLiteral("camera_failed"));
    }
    void deliverNegotiation(const QString &callId, const QString &type)
    {
        Q_EMIT negotiationReady(callId, type, QStringLiteral("v=0 renegotiated"));
    }
    void deliverRemoteVideo(const QString &callId, bool receiving)
    {
        Q_EMIT remoteVideoChanged(callId, receiving);
    }
    bool videoEngine = false;
    QString streamId;
    QStringList prepared;
    QList<bool> cameraRequests;
    QStringList negotiations;

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

class KeyRecordingClient : public RecordingCallClient
{
public:
    struct KeySend {
        QString key;
        int index = -1;
        QStringList devices;
        quint64 op = 0;
    };
    quint64 rtcSendMediaKey(const QString &roomId, const QString &keyBase64,
                            int keyIndex, const QString &targetsJson) override
    {
        Q_UNUSED(roomId);
        const QJsonArray rows =
            QJsonDocument::fromJson(targetsJson.toUtf8()).array();
        if (rows.isEmpty() || refuseSends)
            return 0;
        KeySend send;
        send.key = keyBase64;
        send.index = keyIndex;
        for (const QJsonValue &row : rows) {
            send.devices << row.toObject()
                                .value(QStringLiteral("device_id"))
                                .toString();
        }
        send.devices.sort();
        send.op = ++opCounter;
        keySends.append(send);
        return send.op;
    }
    void answerKeySend(const KeySend &send, bool ok)
    {
        Q_EMIT rtcMediaKeySent(send.op, ok,
                               ok ? QString() : QStringLiteral("network"),
                               ok ? int(send.devices.size()) : 0, send.index);
    }
    void answerKeySendPartial(const KeySend &send, const QStringList &missed)
    {
        QVariantList undelivered;
        for (const QString &device : missed) {
            undelivered.append(QVariantMap{
                { QStringLiteral("user_id"),
                  QLatin1Char('@') + device.toLower()
                      + QStringLiteral(":example.org") },
                { QStringLiteral("device_id"), device } });
        }
        const int delivered = int(send.devices.size() - missed.size());
        Q_EMIT rtcMediaKeySent(send.op, delivered > 0,
                               QStringLiteral("partial"), delivered,
                               send.index, undelivered);
    }
    QList<KeySend> keySends;
    bool refuseSends = false;
};

struct KeyLaneCall {
    KeyRecordingClient client;
    RtcController rtc;
    SfuCallController call;
    const QString room = QStringLiteral("!keys:example.org");

    explicit KeyLaneCall(const QStringList &devices)
    {
        rtc.setClient(&client);
        rtc.setPokeCoalesceMsForTest(0);
        call.setClient(&client);
        call.setRtcController(&rtc);
        RtcSessionData session;
        session.roomId = room;
        for (const QString &device : devices) {
            RtcParticipant member;
            member.userId = userFor(device);
            member.deviceId = device;
            member.rtcIdentity = identityFor(device);
            member.intent = QStringLiteral("audio");
            member.wireFormat = QStringLiteral("session");
            session.participants.append(member);
        }
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        enterCall();
    }
    void enterCall()
    {
        call.setMembershipForTest(room, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));
    }
    static QString userFor(const QString &device)
    {
        return QLatin1Char('@') + device.toLower()
            + QStringLiteral(":example.org");
    }
    static QString identityFor(const QString &device)
    {
        return userFor(device) + QLatin1Char(':') + device;
    }
    void sfuJoin(const QString &device, const QString &sid)
    {
        deliver({ sfuParticipant(identityFor(device), sid, {}) });
    }
    void sfuLeave(const QString &device)
    {
        QVariantMap row = sfuParticipant(identityFor(device), QString(), {});
        row.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
        deliver({ row });
    }
    void deliver(const QVariantList &updates)
    {
        QVERIFY(QMetaObject::invokeMethod(&call, "onSfuParticipants",
                                          Qt::DirectConnection,
                                          Q_ARG(QVariantList, updates)));
    }
    const KeyRecordingClient::KeySend &lastSend() const
    {
        return client.keySends.constLast();
    }
    int newest() const { return call.newestKeyIndexForTest(); }
    int adopted() const { return call.adoptedKeyIndexForTest(); }
};

struct CameraPortalCall {
    RecordingCallClient client;
    SfuCallController call;
    CameraPortal portal;
    QList<int> published;
    QList<bool> publishedOpen;
    int requests = 0;

    CameraPortalCall()
    {
        call.setClient(&client);
        call.setCameraPortal(&portal);
        call.setMembershipForTest(QStringLiteral("!cam:example.org"),
                                  QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setCameraRouteForTest(false,
false,
false);
        call.setCameraPathForTest(
            [this](int fd) {
                published.append(fd);
                publishedOpen.append(fdOpen(fd));
#ifdef Q_OS_UNIX
                if (fd >= 0)
                    ::close(fd);
#endif
            },
            [this] { ++requests; },true,
true);
    }
    static bool fdOpen(int fd)
    {
#ifdef Q_OS_UNIX
        return fd >= 0 && ::fcntl(fd, F_GETFD) != -1;
#else
        Q_UNUSED(fd);
        return false;
#endif
    }
    static int portalFd()
    {
#ifdef Q_OS_UNIX
        int ends[2] = {-1, -1};
        if (::pipe(ends) != 0)
            return -1;
        ::close(ends[1]);
        return ends[0];
#else
        return -1;
#endif
    }
};

const QString kReconnectRoom = QStringLiteral("!reconnect:example.org");
const QString kReconnectFocus = QStringLiteral("https://sfu.example.org");

void connectRecoverableCall(RecordingCallClient &client,
                            SfuCallController &call,
                            const QString &delayId = QString(),
                            const QString &room = kReconnectRoom,
                            bool transportUp = true)
{
    call.setClient(&client);
    const quint64 op = call.beginMembershipPublishForTest(room,
                                                          kReconnectFocus);
    QVERIFY(op != 0);
    client.answerPublish(op, true, delayId);
    QCOMPARE(call.state(), SfuCallController::State::Authorizing);
    QCOMPARE(client.sfuConnects.size(), 1);
    client.emitSfuState(QStringLiteral("authorized"), QString());
    client.emitSfuState(QStringLiteral("signalling"), QString());
    call.setCallStateForTest(SfuCallController::State::Connected);
    if (transportUp) {
        QVERIFY(QMetaObject::invokeMethod(&call, "onEngineTransportState",
                                          Qt::DirectConnection,
                                          Q_ARG(int, 1),
                                          Q_ARG(QString,
                                                QStringLiteral("connected"))));
    }
}

void emitSfuJoin(RecordingCallClient &client, const QString &ownIdentity,
                 const QVariantList &participants)
{
    Q_EMIT client.sfuJoined(ownIdentity, participants, QVariantList(),
                            QByteArray());
}

bool emitTransport(SfuCallController &call, int target, const QString &state)
{
    return QMetaObject::invokeMethod(&call, "onEngineTransportState",
                                     Qt::DirectConnection,
                                     Q_ARG(int, target),
                                     Q_ARG(QString, state));
}

}

static int rowsFor(QAbstractItemModel *model, const QString &identity)
{
    const int role = model->roleNames().key("identity", -1);
    int n = 0;
    for (int r = 0; r < model->rowCount(); ++r)
        if (model->data(model->index(r, 0), role).toString() == identity)
            ++n;
    return n;
}

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
        notify.notificationType = QStringLiteral("ring");
        notify.mentionsRoom = true;
        notify.roomId = roomId;
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = sender;
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        notify.callIntent = QStringLiteral("audio");
        client.emitSignal(notify);
    }

    static CallSignal rtcAnnouncement(const QString &roomId,
                                      const QString &eventId,
                                      const QString &sender,
                                      const QString &notificationType,
                                      bool mentionsRoom,
                                      const QStringList &mentionUserIds = {})
    {
        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.roomId = roomId;
        notify.eventId = eventId;
        notify.sender = sender;
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        notify.callIntent = QStringLiteral("audio");
        notify.notificationType = notificationType;
        notify.mentionsRoom = mentionsRoom;
        notify.mentionUserIds = mentionUserIds;
        return notify;
    }
    static RoomInfo directRoom(const QString &roomId,
                               const QStringList &targets)
    {
        RoomInfo room;
        room.id = roomId;
        room.isDirect = true;
        room.directUserIds = targets;
        room.directUserId = targets.value(0);
        return room;
    }

private Q_SLOTS:
    void streamInfoIsOptInAndClearedOnLeave()
    {
        SfuCallController controller;
        QVERIFY(!controller.streamInfoVisible());
        QVERIFY(controller.streamInfo().isEmpty());
        controller.setStreamInfoVisible(true);
        QVERIFY(!controller.streamInfoVisible());
        controller.setCallStateForTest(SfuCallController::State::Connected);
        controller.setStreamInfoVisible(true);
        QVERIFY(controller.streamInfoVisible());
        controller.setStreamInfoVisible(false);
        QVERIFY(!controller.streamInfoVisible());
        QVERIFY(controller.streamInfo().isEmpty());
        controller.setStreamInfoVisible(true);
        controller.leave();
        QVERIFY(!controller.streamInfoVisible());
        QVERIFY(controller.streamInfo().isEmpty());
    }

    void duplicateAcrossFocusesSurvivesOneDisconnectWithoutKeyRotation()
    {
        KeyLaneCall c({QStringLiteral("ADEV")});
        const QString identity = KeyLaneCall::identityFor(QStringLiteral("ADEV"));
        c.deliver({sfuParticipant(identity, QStringLiteral("publisher-sid"), {})});
        c.call.ingestSubscriberParticipantsForTest(1, {
            sfuParticipant(identity, QStringLiteral("subscriber-sid"), {
                sfuTrack(QStringLiteral("microphone"), QStringLiteral("audio-sid"), false)
            })
        }, true);
        QCOMPARE(c.call.streamIdForIdentityForTest(identity),
                 QStringLiteral("c1:subscriber-sid"));
        QCOMPARE(c.call.trackKeyForSourceForTest(identity,
                 QStringLiteral("microphone")), QStringLiteral("c1:audio-sid"));
        c.call.startKeyLaneForTest();
        const int key = c.newest();
        QVERIFY(key >= 0);
        QVariantMap gone = sfuParticipant(identity, QString(), {});
        gone.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
        c.deliver({gone});
        QCOMPARE(rowsFor(c.call.participantModel(), identity), 1);
        QCOMPARE(c.call.streamIdForIdentityForTest(identity),
                 QStringLiteral("c1:subscriber-sid"));
        QCOMPARE(c.call.trackKeyForSourceForTest(identity,
                 QStringLiteral("microphone")), QStringLiteral("c1:audio-sid"));
        QCOMPARE(c.newest(), key);
    }

    void subscriberReplacementKeepsPublisherPresence()
    {
        KeyLaneCall c({QStringLiteral("ADEV")});
        const QString identity = KeyLaneCall::identityFor(QStringLiteral("ADEV"));
        c.deliver({sfuParticipant(identity, QStringLiteral("publisher-sid"), {
            sfuTrack(QStringLiteral("microphone"), QStringLiteral("publisher-audio"), false)
        })});
        c.call.ingestSubscriberParticipantsForTest(1, {
            sfuParticipant(identity, QStringLiteral("subscriber-sid"), {})
        }, true);
        c.call.startKeyLaneForTest();
        const int key = c.newest();
        QVERIFY(key >= 0);
        c.call.ingestSubscriberParticipantsForTest(1, {}, true);
        QCOMPARE(rowsFor(c.call.participantModel(), identity), 1);
        QCOMPARE(c.call.streamIdForIdentityForTest(identity),
                 QStringLiteral("publisher-sid"));
        QCOMPARE(c.call.trackKeyForSourceForTest(identity,
                 QStringLiteral("microphone")), QStringLiteral("publisher-audio"));
        QCOMPARE(c.newest(), key);
    }

    void duplicateTrackRowsPreferAdvertisedFocus()
    {
        KeyLaneCall c({QStringLiteral("ADEV")});
        const QString identity = KeyLaneCall::identityFor(QStringLiteral("ADEV"));
        const QString preferred = QStringLiteral("https://preferred.example.org");
        RtcSessionData session;
        session.roomId = c.room;
        RtcParticipant member;
        member.userId = KeyLaneCall::userFor(QStringLiteral("ADEV"));
        member.deviceId = QStringLiteral("ADEV");
        member.rtcIdentity = identity;
        member.focusSelection = QStringLiteral("multi_sfu");
        member.focusServiceUrl = preferred;
        session.participants = {member};
        c.rtc.refresh(c.room);
        c.client.answerSession(c.client.lastSessionOp, session);
        c.call.setConnectionFocusUrlsForTest({
            QStringLiteral("https://other.example.org"), preferred});
        c.deliver({sfuParticipant(identity, QStringLiteral("other-sid"), {
            sfuTrack(QStringLiteral("microphone"), QStringLiteral("other-audio"), false)
        })});
        c.call.ingestSubscriberParticipantsForTest(1, {
            sfuParticipant(identity, QStringLiteral("preferred-sid"), {
                sfuTrack(QStringLiteral("microphone"), QStringLiteral("preferred-audio"), false)
            })
        }, true);
        QCOMPARE(c.call.streamIdForIdentityForTest(identity),
                 QStringLiteral("c1:preferred-sid"));
        QCOMPARE(c.call.trackKeyForSourceForTest(identity,
                 QStringLiteral("microphone")), QStringLiteral("c1:preferred-audio"));
    }

    void noOwnFocusUsesOldestForMultiSfuPeers()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        const QString room = QStringLiteral("!fallback:example.org");
        const QString oldest = QStringLiteral("https://old.example.org");
        const QString second = QStringLiteral("https://second.example.org");
        rtc.discover(room);
        client.answerTransports({});
        RtcSessionData session;
        session.roomId = room;
        session.focusServiceUrl = oldest;
        RtcParticipant a;
        a.rtcIdentity = QStringLiteral("@a:example.org:A");
        a.focusSelection = QStringLiteral("multi_sfu");
        a.focusServiceUrl = oldest;
        a.expiresAtMs = std::numeric_limits<qint64>::max();
        RtcParticipant b;
        b.rtcIdentity = QStringLiteral("@b:example.org:B");
        b.focusSelection = QStringLiteral("multi_sfu");
        b.focusServiceUrl = second;
        b.expiresAtMs = std::numeric_limits<qint64>::max();
        session.participants = {a, b};
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        QVERIFY(rtc.callingAvailable());
        QCOMPARE(rtc.focusUrlFor(room), oldest);
        QCOMPARE(rtc.subscriberFocusUrlsFor(room), QStringList({oldest, second}));
        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        const quint64 op = call.beginMembershipPublishForTest(room, oldest);
        QVERIFY(op != 0);
        QCOMPARE(client.publishedMultiSfu.last(), false);
        client.answerPublish(op, true, QString());
        QCOMPARE(client.focusPlans.last(), QStringList({oldest, second}));
    }

    void noOwnFocusLightningToLightningUsesOldest()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        const QString room = QStringLiteral("!lightning:example.org");
        const QString oldest = QStringLiteral("https://peer.example.org");
        rtc.discover(room);
        client.answerTransports({});
        RtcSessionData session;
        session.roomId = room;
        session.focusServiceUrl = oldest;
        RtcParticipant lightning;
        lightning.rtcIdentity = QStringLiteral("@peer:example.org:DEV");
        lightning.focusSelection = QStringLiteral("multi_sfu");
        lightning.focusServiceUrl = oldest;
        lightning.expiresAtMs = std::numeric_limits<qint64>::max();
        session.participants = {lightning};
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        QCOMPARE(rtc.focusUrlFor(room), oldest);
        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        const quint64 op = call.beginMembershipPublishForTest(room, oldest);
        QVERIFY(op != 0);
        QCOMPARE(client.publishedMultiSfu.last(), false);
        client.answerPublish(op, true, QString());
        QCOMPARE(client.focusPlans.last(), QStringList({oldest}));
    }

    void expiredCallMembershipCannotRequestRemoteFocus()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        const QString room = QStringLiteral("!expired:example.org");
        const QString own = QStringLiteral("https://own.example.org");
        const QString remote = QStringLiteral("https://remote.example.org");
        rtc.discover(room);
        client.answerTransports({own});
        RtcSessionData session;
        session.roomId = room;
        session.focusServiceUrl = remote;
        RtcParticipant stale;
        stale.focusSelection = QStringLiteral("multi_sfu");
        stale.focusServiceUrl = remote;
        stale.expiresAtMs = QDateTime::currentMSecsSinceEpoch() - 1;
        session.participants = {stale};
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        QCOMPARE(rtc.focusUrlFor(room), own);
        QVERIFY(rtc.subscriberFocusUrlsFor(room).isEmpty());
        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        const quint64 op = call.beginMembershipPublishForTest(room, own);
        QVERIFY(op != 0);
        client.answerPublish(op, true, QString());
        QCOMPARE(client.focusPlans.last(), QStringList({own}));
    }

    void aFocusSpelledWithATrailingSlashIsTheSameConnection()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        const QString room = QStringLiteral("!slash:example.org");
        const QString own = QStringLiteral("https://own.example.org");
        const QString slashed = QStringLiteral("https://own.example.org/");
        QVERIFY(RtcController::sameFocusUrl(own, slashed));
        QVERIFY(RtcController::sameFocusUrl(
            own, QStringLiteral("https://own.example.org:443/")));
        QVERIFY(RtcController::sameFocusUrl(
            own, QStringLiteral("https://own.example.org/prefix/../")));
        QVERIFY(!RtcController::sameFocusUrl(
            QStringLiteral("https://own.example.org/a"),
            QStringLiteral("https://own.example.org/a/")));
        QVERIFY(!RtcController::sameFocusUrl(
            own, QStringLiteral("https://own.example.org:8443")));
        rtc.discover(room);
        client.answerTransports({own});
        RtcSessionData session;
        session.roomId = room;
        RtcParticipant b;
        b.rtcIdentity = QStringLiteral("@b:example.org:B");
        b.focusSelection = QStringLiteral("multi_sfu");
        b.focusServiceUrl = slashed;
        b.expiresAtMs = std::numeric_limits<qint64>::max();
        session.participants = {b};
        session.focusServiceUrl = slashed;
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        const quint64 op = call.beginMembershipPublishForTest(room,
                                                               rtc.focusUrlFor(room));
        client.answerPublish(op, true, QString());
        QCOMPARE(client.focusPlans.last().size(), 1);
        QCOMPARE(call.connectionFocusUrlsForTest().size(), 1);
        call.leave();
    }

    void multiSfuPlanCreatesPublisherAndTwoFocusRoutes()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        const QString room = QStringLiteral("!multi:example.org");
        const QString own = QStringLiteral("https://own.example.org");
        const QString other = QStringLiteral("https://other.example.org");
        rtc.discover(room);
        client.answerTransports({own});
        RtcSessionData session;
        session.roomId = room;
        RtcParticipant a;
        a.rtcIdentity = QStringLiteral("@a:example.org:A");
        a.focusSelection = QStringLiteral("multi_sfu");
        a.focusServiceUrl = own;
        a.expiresAtMs = std::numeric_limits<qint64>::max();
        RtcParticipant b;
        b.rtcIdentity = QStringLiteral("@b:other.org:B");
        b.focusSelection = QStringLiteral("multi_sfu");
        b.focusServiceUrl = other;
        b.expiresAtMs = std::numeric_limits<qint64>::max();
        session.participants = {a, b};
        session.focusServiceUrl = other;
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        QCOMPARE(rtc.focusUrlFor(room), own);
        QCOMPARE(rtc.subscriberFocusUrlsFor(room), QStringList({own, other}));

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        const quint64 op = call.beginMembershipPublishForTest(room,
                                                               rtc.focusUrlFor(room));
        client.answerPublish(op, true, QString());
        QCOMPARE(client.focusPlans.last(), QStringList({own, other}));
        QCOMPARE(call.connectionFocusUrlsForTest(), QStringList({own, other}));
        call.leave();
        QVERIFY(call.connectionFocusUrlsForTest().isEmpty());
        QVERIFY(client.sfuDisconnects > 0);
    }

    void equivalentRemoteFocusSpellingKeepsTheCallConnected()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        const QString room = QStringLiteral("!multi:example.org");
        const QString own = QStringLiteral("https://own.example.org");
        const QString other = QStringLiteral("https://other.example.org");
        rtc.discover(room);
        client.answerTransports({own});
        RtcSessionData session;
        session.roomId = room;
        RtcParticipant remote;
        remote.rtcIdentity = QStringLiteral("@b:other.org:B");
        remote.focusSelection = QStringLiteral("multi_sfu");
        remote.focusServiceUrl = other;
        remote.expiresAtMs = std::numeric_limits<qint64>::max();
        session.participants = {remote};
        session.focusServiceUrl = other;
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        const quint64 op = call.beginMembershipPublishForTest(room, own);
        client.answerPublish(op, true, QString());
        Q_EMIT client.sfuConnectionEvent(QVariantMap{
            {QStringLiteral("type"), QStringLiteral("sfu_state")},
            {QStringLiteral("generation"), 7},
            {QStringLiteral("connection_id"), 0},
        });
        QCOMPARE(call.sfuGenerationForTest(), quint64(7));
        call.setCallStateForTest(SfuCallController::State::Connected);
        QCOMPARE(call.connectionFocusUrlsForTest(), QStringList({own, other}));

        for (const QString &spelling : {other + QStringLiteral("/"),
                 other + QStringLiteral(":443/"),
                 other + QStringLiteral("/prefix/../")}) {
            session.participants[0].focusServiceUrl = spelling;
            rtc.refresh(room);
            client.answerSession(client.lastSessionOp, session);
            QCOMPARE(call.state(), SfuCallController::State::Connected);
            QCOMPARE(call.connectionFocusUrlsForTest(), QStringList({own, other}));
        }
        session.participants[0].focusServiceUrl = QStringLiteral("https://new.example.org");
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        QCOMPARE(call.state(), SfuCallController::State::Reconnecting);
    }

    void foreignPublisherDenialMovesHomeOnceAndRejectsStaleGeneration()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        const QString room = QStringLiteral("!mixed:example.org");
        const QString own = QStringLiteral("https://own.example.org");
        const QString foreign = QStringLiteral("https://old.example.org");
        rtc.discover(room);
        client.answerTransports({own});
        RtcSessionData session;
        session.roomId = room;
        session.focusServiceUrl = foreign;
        RtcParticipant legacy;
        legacy.rtcIdentity = QStringLiteral("@old:example.org:OLD");
        legacy.focusSelection = QStringLiteral("oldest_membership");
        legacy.focusServiceUrl = foreign;
        legacy.expiresAtMs = std::numeric_limits<qint64>::max();
        session.participants = {legacy};
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        QCOMPARE(rtc.focusUrlFor(room), foreign);

        SfuCallController call;
        call.setClient(&client);
        call.setRtcController(&rtc);
        const quint64 op = call.beginMembershipPublishForTest(room, foreign);
        client.answerPublish(op, true, QString());
        QCOMPARE(client.focusPlans.size(), 1);
        client.emitSfuState(QStringLiteral("failed"),
                            QStringLiteral("publish_denied"));
        QCOMPARE(client.focusPlans.size(), 2);
        QCOMPARE(client.focusPlans.last(), QStringList({own, foreign}));

        Q_EMIT client.sfuConnectionEvent(QVariantMap{
            {QStringLiteral("type"), QStringLiteral("sfu_state")},
            {QStringLiteral("generation"), 7},
            {QStringLiteral("connection_id"), 0},
        });
        QCOMPARE(call.sfuGenerationForTest(), quint64(7));
        client.emitSfuState(QStringLiteral("failed"),
                            QStringLiteral("publish_denied"));
        QCOMPARE(client.focusPlans.size(), 2);
        QCOMPARE(call.state(), SfuCallController::State::Failed);
        QCOMPARE(call.sfuGenerationForTest(), quint64(0));
        Q_EMIT client.sfuConnectionEvent(QVariantMap{
            {QStringLiteral("type"), QStringLiteral("sfu_state")},
            {QStringLiteral("generation"), 7},
            {QStringLiteral("connection_id"), 0},
        });
        QCOMPARE(call.sfuGenerationForTest(), quint64(0));
    }

    void twoSubscriberConnectionsKeepEqualSidsSeparate()
    {
        SfuCallController call;
        call.setCallStateForTest(SfuCallController::State::Connected);
        const QString a = QStringLiteral("@a:x:A");
        const QString b = QStringLiteral("@b:y:B");
        const QVariantList first{
            sfuParticipant(a, QStringLiteral("PA_SAME"), {})};
        const QVariantList second{
            sfuParticipant(b, QStringLiteral("PA_SAME"), {})};
        QVERIFY(QMetaObject::invokeMethod(&call, "updateSubscriberParticipants",
            Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(QVariantList, first),
            Q_ARG(bool, true)));
        QVERIFY(QMetaObject::invokeMethod(&call, "updateSubscriberParticipants",
            Qt::DirectConnection, Q_ARG(int, 2), Q_ARG(QVariantList, second),
            Q_ARG(bool, true)));
        QCOMPARE(call.streamIdForIdentityForTest(a), QStringLiteral("c1:PA_SAME"));
        QCOMPARE(call.streamIdForIdentityForTest(b), QStringLiteral("c2:PA_SAME"));
    }

    void aKeyThatArrivesBeforeTheCallIsParkedRatherThanDiscarded()
    {
        SfuCallController call;
        const QString key = QString::fromUtf8(QByteArray(32, 'k').toBase64());
        QMetaObject::invokeMethod(
            &call, "onMediaKeyReceived", Qt::DirectConnection,
            Q_ARG(QString, QString()), Q_ARG(QString, QStringLiteral("@a:x")),
            Q_ARG(QString, QStringLiteral("DEV1")), Q_ARG(int, 0),
            Q_ARG(QString, key));
        QCOMPARE(call.parkedKeyCountForTest(), 1);

        call.setCallStateForTest(SfuCallController::State::Connected);
        QCOMPARE(call.parkedKeyCountForTest(), 1);
    }

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

        park(QStringLiteral("DEV2"), 256, good);
        park(QStringLiteral("DEV5"), -1, good);
        park(QStringLiteral("DEV6"), 100000, good);
        park(QStringLiteral("DEV3"), 0, QString(300, QLatin1Char('A')));
        park(QStringLiteral("DEV4"), 0,
             QString::fromUtf8(QByteArray(7, 'k').toBase64()));
        QCOMPARE(call.parkedKeyCountForTest(), 1);
    }

    void aMediaKeyAtAHighIndexIsKeptRatherThanDiscarded()
    {
        SfuCallController call;
        const QString key = QString::fromUtf8(QByteArray(16, 'k').toBase64());
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
        QMetaObject::invokeMethod(
            &call, "onMediaKeyReceived", Qt::DirectConnection,
            Q_ARG(QString, QString()),
            Q_ARG(QString, QStringLiteral("@element:x")),
            Q_ARG(QString, QStringLiteral("ECDEV2")), Q_ARG(int, 256),
            Q_ARG(QString, key));
        QCOMPARE(call.parkedKeyCountForTest(), 3);
    }

    void theInCallMeterFollowsTheCaptureAtABoundedRate()
    {
        SfuCallController call;
        QSignalSpy changed(&call, &SfuCallController::microphoneLevelChanged);

        call.noteMicrophoneLevelAt(-6.0, 0);
        QCOMPARE(call.microphoneLevel(), 0.0);
        QCOMPARE(changed.count(), 0);

        call.setCallStateForTest(SfuCallController::State::Connected);
        call.noteMicrophoneLevelAt(-30.0, 1000);
        QCOMPARE(call.microphoneLevel(),
                 lightning::calls::meterFraction(-30.0));
        call.noteMicrophoneLevelAt(-350.0, 2000);
        QCOMPARE(call.microphoneLevel(), 0.0);

        changed.clear();
        for (int i = 0; i < 200; ++i)
            call.noteMicrophoneLevelAt(-59.0 + i * 0.25, 3000 + i * 5);
        const int perSecond = 1000 / int(lightning::calls::kMeterMinIntervalMs);
        QVERIFY2(changed.count() <= perSecond + 1,
                 qPrintable(QString::number(changed.count())));
        QVERIFY2(changed.count() >= perSecond - 2,
                 qPrintable(QString::number(changed.count())));
        call.noteMicrophoneLevelAt(-50.0, 4100);
        QCOMPARE(call.microphoneLevel(),
                 lightning::calls::meterFraction(-59.0 + 199 * 0.25));

        call.noteMicrophoneLevelAt(-10.0, 10000);
        QVERIFY(call.microphoneLevel() > 0.0);
        call.setMicrophoneMuted(true);
        QCOMPARE(call.microphoneLevel(), 0.0);
        call.noteMicrophoneLevelAt(-10.0, 11000);
        QCOMPARE(call.microphoneLevel(), 0.0);
        call.setMicrophoneMuted(false);
        call.noteMicrophoneLevelAt(-10.0, 12000);
        QVERIFY(call.microphoneLevel() > 0.0);
    }

    void aRefusedJoinLeavesTheRunningCallAlone()
    {
        struct SfuClient : RecordingCallClient {
            bool supportsSfu() const override { return true; }
        };
        SfuClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!first:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        QSignalSpy changed(&call, &SfuCallController::stateChanged);

        QVERIFY(!call.join(QStringLiteral("!second:x"), false));
        QCOMPARE(call.state(), SfuCallController::State::Connected);
        QVERIFY(call.active());
        QCOMPARE(call.roomId(), QStringLiteral("!first:x"));
        QCOMPARE(changed.count(), 0);
        QVERIFY(call.lastError().isEmpty());
        QCOMPARE(failed.count(), 1);
        QVERIFY(!failed.first().first().toString().isEmpty());

        SfuCallController bare;
        bare.setCallStateForTest(SfuCallController::State::Connected);
        QVERIFY(!bare.join(QStringLiteral("!second:x"), false));
        QCOMPARE(bare.state(), SfuCallController::State::Connected);

        call.leave();
        const quint64 accepted = call.beginMembershipPublishForTest(
            QStringLiteral("!second:x"),
            QStringLiteral("https://sfu.example.org"));
        client.answerPublish(accepted, true, QString());
        QCOMPARE(call.state(), SfuCallController::State::Authorizing);
        QCOMPARE(failed.count(), 2);
        QVERIFY2(failed.at(1).at(0).toString().isEmpty(),
                 "a refusal made while a call was active is never withdrawn, "
                 "so the status strip shows it through the next call");

        call.leave();
        QVERIFY(!call.join(QStringLiteral("!second:x"), false));
        QCOMPARE(call.state(), SfuCallController::State::Failed);
        QVERIFY(!call.lastError().isEmpty());
    }

    void aLostPlaybackIsReportedWithoutEndingTheCall()
    {
        SfuCallController call;
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        const auto report = [&call](bool lost) {
            QVERIFY(QMetaObject::invokeMethod(&call, "onRemotePlaybackFailed",
                                              Qt::DirectConnection,
                                              Q_ARG(bool, lost)));
        };
        report(true);
        report(true);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!failed.at(0).at(0).toString().isEmpty());
        QCOMPARE(call.state(), SfuCallController::State::Connected);

        report(false);
        QCOMPARE(failed.count(), 2);
        QVERIFY(failed.at(1).at(0).toString().isEmpty());

        call.leave();
        report(true);
        QCOMPARE(failed.count(), 2);
    }

    void theLostPlaybackNoticeEndsWithItsCall()
    {
        SfuCallController call;
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        QVERIFY(QMetaObject::invokeMethod(&call, "onRemotePlaybackFailed",
                                          Qt::DirectConnection,
                                          Q_ARG(bool, true)));
        QCOMPARE(failed.count(), 1);
        call.leave();
        QCOMPARE(failed.count(), 2);
        QVERIFY(failed.at(1).at(0).toString().isEmpty());
    }

    void theMicrophoneNoticeEndsWithItsCall()
    {
        SfuCallController call;
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        Q_EMIT call.callFailed(QStringLiteral("Your microphone isn't available."));
        QCOMPARE(failed.count(), 1);
        call.leave();
        QCOMPARE(failed.count(), 2);
        QVERIFY(failed.at(1).at(0).toString().isEmpty());
    }

    void aWithdrawalDoesNotClearSomebodyElsesNotice()
    {
        SfuCallController call;
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        const auto report = [&call](bool lost) {
            QVERIFY(QMetaObject::invokeMethod(&call, "onRemotePlaybackFailed",
                                              Qt::DirectConnection,
                                              Q_ARG(bool, lost)));
        };
        report(true);
        QCOMPARE(failed.count(), 1);
        Q_EMIT call.callFailed(QStringLiteral("Your microphone isn't available."));
        QCOMPARE(failed.count(), 2);
        report(false);
        QCOMPARE(failed.count(), 2);
    }

    void aDeliveredKeyIndexDoesNotOutliveItsCall()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QCOMPARE(call.deliveredKeyIndexForTest(), -1);

        call.noteMediaKeySendForTest(41);
        Q_EMIT client.rtcMediaKeySent(41, true, QString(), 1, 3);
        QCOMPARE(call.deliveredKeyIndexForTest(), 3);

        call.leave();
        QCOMPARE(call.deliveredKeyIndexForTest(), -1);
    }

    void aKeySendAnswerFromThePreviousCallChangesNothing()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.noteMediaKeySendForTest(41);
        call.leave();

        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        Q_EMIT client.rtcMediaKeySent(41, true, QString(), 1, 3);
        QCOMPARE(call.deliveredKeyIndexForTest(), -1);

        call.noteMediaKeySendForTest(42);
        Q_EMIT client.rtcMediaKeySent(42, true, QString(), 1, 1);
        QCOMPARE(call.deliveredKeyIndexForTest(), 1);
    }

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
        for (int i = 0; i < 40; ++i)
            park(QStringLiteral("@flood:x"), i % 16);
        QVERIFY2(call.parkedKeyCountForTest() <= 8,
                 qPrintable(QStringLiteral("parked=%1")
                                .arg(call.parkedKeyCountForTest())));
        QVERIFY(call.hasParkedKeyForTest(QStringLiteral("@victim:x")));
    }

    void hashedSessionIdentityResolvesTheTileAndKeyTarget()
    {
        KeyLaneCall c({});
        RtcParticipant alice;
        alice.userId = QStringLiteral("@alice:example.com");
        alice.deviceId = QStringLiteral("ALICEDEV");
        alice.rtcIdentity = QStringLiteral("@alice:example.com:ALICEDEV");
        alice.hashedRtcIdentity =
            QStringLiteral("M3VnsLXrkbxIDINXO64u8PZzFyD9ZYFiQULR2dshWYs");
        alice.displayName = QStringLiteral("Alice");
        alice.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = c.room;
        session.participants = { alice };
        c.rtc.refresh(c.room);
        c.client.answerSession(c.client.lastSessionOp, session);

        c.deliver({ sfuParticipant(alice.hashedRtcIdentity,
                                   QStringLiteral("PA_ALICE"), {}) });
        QCOMPARE(c.rtc.participantForIdentity(c.room, alice.hashedRtcIdentity)
                     .value(QStringLiteral("deviceId")).toString(),
                 alice.deviceId);
        const int row = participantRowFor(c.call.participantModel(),
                                          alice.hashedRtcIdentity);
        QVERIFY(row >= 0);
        QCOMPARE(participantRole(c.call.participantModel(), row,
                                 CallParticipantModel::DisplayNameRole).toString(),
                 alice.displayName);
        QCOMPARE(participantRole(c.call.participantModel(), row,
                                 CallParticipantModel::DeviceIdRole).toString(),
                 alice.deviceId);
        c.call.startKeyLaneForTest();
        QCOMPARE(c.client.keySends.size(), 1);
        QCOMPARE(c.lastSend().devices, QStringList{ alice.deviceId });
    }

    void copiedMemberIdCannotCaptureAnotherSendersHash()
    {
        const auto hash = [](const QByteArray &json) {
            return QString::fromLatin1(QCryptographicHash::hash(
                json, QCryptographicHash::Sha256).toBase64(
                    QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals));
        };
        RtcController rtc;
        RecordingCallClient client;
        rtc.setClient(&client);
        RtcParticipant mallory;
        mallory.userId = QStringLiteral("@mallory:x");
        mallory.deviceId = QStringLiteral("MDEV");
        mallory.rtcIdentity = QStringLiteral("@mallory:x:MDEV");
        mallory.hashedRtcIdentity = hash(
            "[\"@mallory:x\",\"@alice:x:ALICEDEV\"]");
        mallory.wireFormat = QStringLiteral("session");
        RtcSessionData session;
        session.roomId = QStringLiteral("!forgery:x");
        session.participants = { mallory };
        rtc.refresh(session.roomId);
        client.answerSession(client.lastSessionOp, session);
        QVERIFY(rtc.participantForIdentity(session.roomId,
            hash("[\"@alice:x\",\"@alice:x:ALICEDEV\"]")).isEmpty());
        QCOMPARE(rtc.participantForIdentity(session.roomId,
            mallory.hashedRtcIdentity).value(QStringLiteral("deviceId")).toString(),
            mallory.deviceId);
    }

    void duplicateHashedIdentityDoesNotChooseOneDevice()
    {
        RecordingCallClient client;
        RtcController rtc;
        rtc.setClient(&client);
        RtcParticipant laptop;
        laptop.userId = QStringLiteral("@alice:x");
        laptop.deviceId = QStringLiteral("LAPTOP");
        laptop.rtcIdentity = QStringLiteral("@alice:x:LAPTOP");
        laptop.hashedRtcIdentity = QStringLiteral("shared-hash");
        laptop.wireFormat = QStringLiteral("session");
        RtcParticipant phone = laptop;
        phone.deviceId = QStringLiteral("PHONE");
        phone.rtcIdentity = QStringLiteral("@alice:x:PHONE");
        RtcSessionData session;
        session.roomId = QStringLiteral("!duplicate:x");
        session.participants = { laptop, phone };
        rtc.refresh(session.roomId);
        client.answerSession(client.lastSessionOp, session);
        QVERIFY(rtc.participantForIdentity(session.roomId,
            laptop.hashedRtcIdentity).isEmpty());
    }

    void aJoinerIsSentTheKeyInUseAndNothingRotates()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        QVERIFY(c.client.keySends.isEmpty());
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        QVERIFY(first >= 0 && first < 16);
        QCOMPARE(c.adopted(), first);
        QCOMPARE(c.client.keySends.size(), 1);
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("ADEV") });

        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        QVERIFY2(c.newest() == first,
                 "a join minted a new key: every peer loses our media until "
                 "it arrives (DEFECT 9)");
        QCOMPARE(c.adopted(), first);
        QCOMPARE(c.call.pendingKeySwitchMsForTest(), -1);
        QCOMPARE(c.client.keySends.size(), 2);
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("BDEV") });
        QCOMPARE(c.lastSend().index, first);
        QVERIFY(c.lastSend().key == c.client.keySends.at(0).key);

        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.client.keySends.size(), 2);
    }

    // Sable (sable-core) drops a media key that is not 16 raw bytes, so a
    // Sable participant heard everyone but us while Element, which accepts
    // any length, heard us fine (live, 2026-10-13). Every key we mint, the
    // first, a joiner's rotation and a leaver's, is the reference 16 bytes.
    void everyMintedMediaKeyIsSixteenBytes()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(100, 0, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        const int second = c.newest();
        QVERIFY(second != first);
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
        c.sfuLeave(QStringLiteral("BDEV"));
        QVERIFY(c.newest() != second);
        QVERIFY2(c.client.keySends.size() >= 3,
                 qPrintable(QStringLiteral("expected a first key and two "
                                           "rotations, got %1 sends")
                                .arg(c.client.keySends.size())));
        QSet<QString> distinct;
        for (const KeyRecordingClient::KeySend &send : c.client.keySends) {
            const auto decoded = QByteArray::fromBase64Encoding(
                send.key.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            QVERIFY(decoded.decodingStatus
                    == QByteArray::Base64DecodingStatus::Ok);
            QVERIFY2(decoded.decoded.size() == 16,
                     qPrintable(QStringLiteral("a minted media key is %1 "
                                               "bytes; Sable drops anything "
                                               "but 16")
                                    .arg(decoded.decoded.size())));
            distinct.insert(send.key);
        }
        QVERIFY(distinct.size() >= 3);
    }

    void aLeaverRotatesButOurFramesSwitchOnlyAfterTheUseKeyDelay()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("CDEV") });
        c.call.setKeyTimingForTest(300, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.sfuJoin(QStringLiteral("CDEV"), QStringLiteral("PA_C1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        QCOMPARE(c.client.keySends.size(), 1);
        QCOMPARE(c.lastSend().devices.size(), 3);

        c.sfuLeave(QStringLiteral("CDEV"));
        const int second = c.newest();
        QVERIFY2(second != first, "a leaver did not rotate the key");
        QCOMPARE(c.client.keySends.size(), 2);
        QCOMPARE(c.lastSend().index, second);
        QCOMPARE(c.lastSend().devices,
                 (QStringList{ QStringLiteral("ADEV"), QStringLiteral("BDEV") }));
        QVERIFY(c.lastSend().key != c.client.keySends.at(0).key);
        QVERIFY2(c.adopted() == first,
                 "our frames switched to the rotated key at once, before any "
                 "peer could hold it (DEFECT 9)");
        QVERIFY(c.call.pendingKeySwitchMsForTest() > 0);
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
        QCOMPARE(c.call.pendingKeySwitchMsForTest(), -1);
        QCOMPARE(c.client.keySends.size(), 2);
    }

    void theDefaultUseKeyDelayCoversTheMeasuredDeliveryTime()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        c.sfuLeave(QStringLiteral("BDEV"));
        const int fallback = c.call.pendingKeySwitchMsForTest();
        QVERIFY2(fallback > 5000 && fallback <= 15000,
                 qPrintable(QStringLiteral("unanswered: switch in %1 ms")
                                .arg(fallback)));
        c.client.answerKeySend(c.lastSend(), true);
        const int ms = c.call.pendingKeySwitchMsForTest();
        QVERIFY2(ms > 2000 && ms <= 5000,
                 qPrintable(QStringLiteral("switch in %1 ms").arg(ms)));
    }

    void aSwitchWaitsForTheSendToThePeersToBeAnswered()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("CDEV") });
        c.call.setKeyTimingForTest(100, 60000, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.sfuJoin(QStringLiteral("CDEV"), QStringLiteral("PA_C1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        c.sfuLeave(QStringLiteral("CDEV"));
        const int second = c.newest();
        QVERIFY(second != first);
        const KeyRecordingClient::KeySend rollout = c.lastSend();
        QTest::qWait(400);
        QVERIFY2(c.adopted() == first,
                 "switched before the send was answered: the time the send "
                 "spends before its PUT comes out of the delay");
        c.client.answerKeySend(rollout, true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
    }

    void aJoinersQuickAnswerDoesNotStartTheSwitch()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("CDEV"), QStringLiteral("DDEV") });
        c.call.setKeyTimingForTest(100, 60000, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.sfuJoin(QStringLiteral("CDEV"), QStringLiteral("PA_C1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        c.sfuLeave(QStringLiteral("CDEV"));
        const int second = c.newest();
        const KeyRecordingClient::KeySend rollout = c.lastSend();

        c.sfuJoin(QStringLiteral("DDEV"), QStringLiteral("PA_D1"));
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("DDEV") });
        QCOMPARE(c.lastSend().index, second);
        c.client.answerKeySend(c.lastSend(), true);
        QTest::qWait(400);
        QVERIFY2(c.adopted() == first,
                 "a joiner's answer started the switch while the send to A "
                 "and B, who hold the key in use, was still in flight");
        c.client.answerKeySend(rollout, true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
    }

    void aJoinerIsNotKeptWaitingWhenNoHolderWeCanNameIsLeft()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(100, 60000, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("XDEV"), QStringLiteral("PA_X1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("ADEV") });

        c.sfuLeave(QStringLiteral("ADEV"));
        const int second = c.newest();
        QVERIFY(second != first);
        QCOMPARE(c.adopted(), first);

        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("BDEV") });
        QCOMPARE(c.lastSend().index, second);
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_VERIFY2_WITH_TIMEOUT(c.adopted() == second,
                                  "a joiner waited out the fallback although "
                                  "no holder we can name was left to wait "
                                  "for",
                                  3000);
    }

    void anAnswerForAnOlderKeyDoesNotStartTheSwitch()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("CDEV"), QStringLiteral("DDEV") });
        c.call.setKeyTimingForTest(100, 60000, 300);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.sfuJoin(QStringLiteral("CDEV"), QStringLiteral("PA_C1"));
        c.sfuJoin(QStringLiteral("DDEV"), QStringLiteral("PA_D1"));
        c.call.startKeyLaneForTest();
        c.sfuLeave(QStringLiteral("DDEV"));
        const int second = c.newest();
        const KeyRecordingClient::KeySend secondSend = c.lastSend();
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);

        c.call.setKeyTimingForTest(100, 60000, 60000);
        c.sfuLeave(QStringLiteral("CDEV"));
        const int third = c.newest();
        QVERIFY(third != second);
        const KeyRecordingClient::KeySend thirdSend = c.lastSend();
        QCOMPARE(thirdSend.index, third);
        c.client.answerKeySend(secondSend, true);
        QTest::qWait(400);
        QVERIFY2(c.adopted() == second,
                 "the answer for the key in use started the switch to the "
                 "next one, whose own send was never answered");
        c.client.answerKeySend(thirdSend, true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), third, 3000);
    }

    void aLostAnswerCannotHoldTheOldKeyForEver()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("CDEV") });
        c.call.setKeyTimingForTest(60000, 60000, 300);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.sfuJoin(QStringLiteral("CDEV"), QStringLiteral("PA_C1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        c.sfuLeave(QStringLiteral("CDEV"));
        const int second = c.newest();
        QVERIFY(second != first);
        QCOMPARE(c.adopted(), first);
        QVERIFY2(c.call.pendingKeySwitchMsForTest() > 0,
                 "nothing is armed to switch when no answer comes");
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
        QCOMPARE(c.call.pendingKeySwitchMsForTest(), -1);
    }

    void aJoinerAfterTheGraceGetsAFreshKeyAndNobodyIsCutOff()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(300, 0);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();

        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        const int second = c.newest();
        QVERIFY2(second != first,
                 "a joiner after the grace was handed a key that encrypted "
                 "what we sent before it arrived");
        QCOMPARE(c.lastSend().index, second);
        QCOMPARE(c.lastSend().devices,
                 (QStringList{ QStringLiteral("ADEV"), QStringLiteral("BDEV") }));
        QCOMPARE(c.adopted(), first);
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
    }

    void aPartialKeyDeliveryIsSentAgainToTheDevicesItMissed()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(100, 60000, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        QCOMPARE(c.lastSend().devices,
                 (QStringList{ QStringLiteral("ADEV"), QStringLiteral("BDEV") }));
        const qsizetype sends = c.client.keySends.size();
        c.client.answerKeySendPartial(c.lastSend(),
                                      { QStringLiteral("BDEV") });

        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QVERIFY2(c.client.keySends.size() == sends + 1,
                 "B never got the key and was not sent it again: the partial "
                 "answer counted it as a holder");
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("BDEV") });
        QCOMPARE(c.lastSend().index, first);
        c.client.answerKeySend(c.lastSend(), true);
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.client.keySends.size(), sends + 1);
    }

    void aDeviceTheKeyKeepsMissingIsReSentWithABackoff()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(100, 60000, 60000);
        c.call.setKeyResendBackoffBaseMsForTest(300);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        const QStringList missB{ QStringLiteral("BDEV") };
        c.client.answerKeySendPartial(c.lastSend(), missB);

        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.lastSend().devices, missB);
        const qsizetype afterFirstRetry = c.client.keySends.size();
        c.client.answerKeySendPartial(c.lastSend(), missB);
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QVERIFY2(c.client.keySends.size() == afterFirstRetry,
                 "a device that missed the key twice was sent it again at once");
        QVERIFY(c.call.keyResendsHeldBackForTest() >= 1);
        QTest::qWait(350);
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.client.keySends.size(), afterFirstRetry + 1);
        QCOMPARE(c.lastSend().devices, missB);
        c.client.answerKeySendPartial(c.lastSend(), missB);
        QTest::qWait(350);
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.client.keySends.size(), afterFirstRetry + 1);
        QTest::qWait(350);
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.client.keySends.size(), afterFirstRetry + 2);
        c.client.answerKeySend(c.lastSend(), true);
        c.sfuLeave(QStringLiteral("ADEV"));
        QCOMPARE(c.lastSend().devices, missB);
        c.client.answerKeySendPartial(c.lastSend(), missB);
        const qsizetype afterReset = c.client.keySends.size();
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QVERIFY2(c.client.keySends.size() == afterReset + 1,
                 "a delivery did not reset the backoff: the next miss waited");
        QCOMPARE(c.lastSend().devices, missB);
    }

    void aWholeSendFailureHoldsNoDeviceBack()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(100, 60000, 60000);
        c.call.setKeyResendBackoffBaseMsForTest(10000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        for (int i = 0; i < 3; ++i) {
            const qsizetype before = c.client.keySends.size();
            c.client.answerKeySend(c.lastSend(), false);
            QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
            QVERIFY2(c.client.keySends.size() == before + 1,
                     "a transport failure backed every device off");
        }
        QCOMPARE(c.call.keyResendsHeldBackForTest(), 0);
    }

    void aPartialAnswerThatMissedTheKeyInUseHoldersDoesNotStartTheSwitch()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("DDEV") });
        c.call.setKeyTimingForTest(100, 0, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        c.client.answerKeySend(c.lastSend(), true);

        c.sfuJoin(QStringLiteral("DDEV"), QStringLiteral("PA_D1"));
        const int second = c.newest();
        QVERIFY(second != first);
        QCOMPARE(c.lastSend().devices,
                 (QStringList{ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                               QStringLiteral("DDEV") }));
        c.client.answerKeySendPartial(
            c.lastSend(), { QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        QTest::qWait(400);
        QVERIFY2(c.adopted() == first,
                 "switched on an answer that reached only the joiner: A and B "
                 "never got the new key and were cut off");

        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.lastSend().index, second);
        QCOMPARE(c.lastSend().devices,
                 (QStringList{ QStringLiteral("ADEV"), QStringLiteral("BDEV") }));
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
    }

    void theSecondPersonInHearsUsWithoutWaitingOutTheDelay()
    {
        KeyLaneCall c({ QStringLiteral("ADEV") });
        c.call.setKeyTimingForTest(5000, 0);
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        QCOMPARE(c.adopted(), first);
        QVERIFY(c.client.keySends.isEmpty());

        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        QVERIFY(c.newest() != first);
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("ADEV") });
        QVERIFY2(c.adopted() == c.newest(),
                 "the switch waited although nobody could have been cut off");
        QCOMPARE(c.call.pendingKeySwitchMsForTest(), -1);
    }

    void aLeaveDuringAPendingSwitchRotatesRightAfterIt()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("CDEV"), QStringLiteral("DDEV") });
        c.call.setKeyTimingForTest(300, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.sfuJoin(QStringLiteral("CDEV"), QStringLiteral("PA_C1"));
        c.call.startKeyLaneForTest();
        c.sfuLeave(QStringLiteral("CDEV"));
        const int second = c.newest();
        const KeyRecordingClient::KeySend rollout = c.lastSend();
        const qsizetype sends = c.client.keySends.size();

        c.sfuLeave(QStringLiteral("BDEV"));
        QVERIFY2(c.newest() == second,
                 "a second rotation during the pending switch: the switch "
                 "can be postponed for as long as people keep leaving");
        QCOMPARE(c.client.keySends.size(), sends);
        c.sfuJoin(QStringLiteral("DDEV"), QStringLiteral("PA_D1"));
        QCOMPARE(c.client.keySends.size(), sends + 1);
        QCOMPARE(c.lastSend().index, second);
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("DDEV") });

        c.client.answerKeySend(rollout, true);
        QTRY_VERIFY_WITH_TIMEOUT(c.newest() != second, 3000);
        QCOMPARE(c.lastSend().index, c.newest());
        QCOMPARE(c.lastSend().devices,
                 (QStringList{ QStringLiteral("ADEV"), QStringLiteral("DDEV") }));
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), c.newest(), 3000);
    }

    void aRejoinedDeviceIsSentOurKeyAgain()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(300, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        QCOMPARE(c.client.keySends.size(), 1);

        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B2"));
        QVERIFY2(c.client.keySends.size() == 2,
                 "a device back in a new SFU session was never sent our key");
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("BDEV") });
        QCOMPARE(c.lastSend().index, first);
        QCOMPARE(c.newest(), first);
    }

    void aKeyThatReachedNobodyIsNeverSwitchedToWhileAPeerHoldsOurs()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV"),
                        QStringLiteral("CDEV") });
        c.call.setKeyTimingForTest(100, 60000, 150);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.sfuJoin(QStringLiteral("CDEV"), QStringLiteral("PA_C1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        c.client.answerKeySend(c.lastSend(), true);

        c.client.refuseSends = true;
        c.sfuLeave(QStringLiteral("CDEV"));
        const int second = c.newest();
        QVERIFY(second != first);
        QTest::qWait(300);
        QVERIFY2(c.adopted() == first,
                 "switched to a key the bridge never sent while A and B hold "
                 "the one in use");

        c.client.refuseSends = false;
        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.lastSend().index, second);
        c.client.answerKeySend(c.lastSend(), false);
        QTest::qWait(400);
        QVERIFY2(c.adopted() == first,
                 "switched to a key whose only send was reported failed");
        QCOMPARE(c.call.pendingKeySwitchMsForTest(), -1);

        QVERIFY(QMetaObject::invokeMethod(&c.call, "reconcileKeyLane"));
        QCOMPARE(c.lastSend().index, second);
        QCOMPARE(c.lastSend().devices,
                 (QStringList{ QStringLiteral("ADEV"), QStringLiteral("BDEV") }));
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), second, 3000);
    }

    void aRotatedKeyNoNamedPeerHoldsNeverCutsOffAPeerWeCannotName()
    {
        KeyLaneCall c({ QStringLiteral("ADEV") });
        c.call.setKeyTimingForTest(100, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("ADEV") });

        RtcSessionData session;
        session.roomId = c.room;
        c.client.answerSession(c.client.lastSessionOp, session);
        QVERIFY(c.newest() != first);
        QTest::qWait(300);
        QVERIFY2(c.adopted() == first,
                 "our frames switched to a key nobody holds while a peer the "
                 "SFU still lists may hold the one in use");
    }

    void aDeviceWhoseSendWasReportedFailedStillCountsAsALeaver()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(300, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        const int first = c.newest();
        c.client.answerKeySend(c.lastSend(), false);

        c.sfuLeave(QStringLiteral("BDEV"));
        QVERIFY2(c.newest() != first,
                 "B may hold our key despite the failed report, left, and we "
                 "kept using it");
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("ADEV") });
    }

    void aRejoinContinuesTheKeyIndexInsteadOfReusingThePreviousCalls()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(50, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        c.sfuLeave(QStringLiteral("BDEV"));
        c.client.answerKeySend(c.lastSend(), true);
        QTRY_COMPARE_WITH_TIMEOUT(c.adopted(), c.newest(), 3000);
        QSet<int> firstCall;
        for (const KeyRecordingClient::KeySend &send : c.client.keySends)
            firstCall.insert(send.index);
        QCOMPARE(firstCall.size(), 2);
        const int lastOfFirstCall = c.newest();

        c.call.leave();
        QCOMPARE(c.newest(), -1);
        QCOMPARE(c.adopted(), -1);
        QCOMPARE(c.call.deliveredKeyIndexForTest(), -1);
        c.enterCall();
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A2"));
        c.call.startKeyLaneForTest();
        QVERIFY2(!firstCall.contains(c.newest()),
                 qPrintable(QStringLiteral("the rejoin's first key reused "
                                           "index %1 of the previous call")
                                .arg(c.newest())));
        QCOMPARE(c.newest(), (lastOfFirstCall + 1) % 16);
        QCOMPARE(c.adopted(), c.newest());
    }

    void ourKeyIndexWrapsAtSixteenAndAdvancesByOne()
    {
        KeyLaneCall c({ QStringLiteral("ADEV") });
        c.call.setKeyTimingForTest(5000, 60000);
        c.call.startKeyLaneForTest();
        QList<int> indices{ c.newest() };
        for (int i = 0; i < 20; ++i) {
            c.sfuJoin(QStringLiteral("ADEV"),
                      QStringLiteral("PA_A%1").arg(i));
            c.sfuLeave(QStringLiteral("ADEV"));
            indices.append(c.newest());
            QCOMPARE(c.adopted(), c.newest());
        }
        for (int i = 0; i < indices.size(); ++i) {
            QVERIFY(indices.at(i) >= 0 && indices.at(i) < 16);
            if (i > 0)
                QCOMPARE(indices.at(i), (indices.at(i - 1) + 1) % 16);
        }
    }

    void anUnswitchedKeyNeverTakesTheIndexOurFramesUse()
    {
        KeyLaneCall c({ QStringLiteral("ADEV"), QStringLiteral("BDEV") });
        c.call.setKeyTimingForTest(5000, 0, 60000);
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.sfuJoin(QStringLiteral("BDEV"), QStringLiteral("PA_B1"));
        c.call.startKeyLaneForTest();
        const int inUse = c.adopted();
        QVERIFY(inUse >= 0);
        c.client.refuseSends = true;
        QSet<int> minted;
        for (int i = 0; i < 20; ++i) {
            const int before = c.newest();
            c.sfuJoin(QStringLiteral("BDEV"),
                      QStringLiteral("PA_B%1").arg(i + 2));
            QVERIFY2(c.newest() != before, "the join did not rotate");
            QCOMPARE(c.adopted(), inUse);
            QVERIFY2(c.newest() != inUse,
                     qPrintable(QStringLiteral("key %1 took index %2, which "
                                               "our frames still use")
                                    .arg(i + 1)
                                    .arg(inUse)));
            minted.insert(c.newest());
        }
        QCOMPARE(minted.size(), 15);
    }

    void aCameraNoticeIsWithdrawnWhenALaterCameraIsAccepted()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        call.setCameraCidForTest(QStringLiteral("cam-1"));
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("cam-1")),
            Q_ARG(QString, QStringLiteral("camera_unavailable"))));
        QCOMPARE(failed.count(), 1);
        QVERIFY(!failed.at(0).at(0).toString().isEmpty());
        QVERIFY(!call.cameraOn());

        Q_EMIT client.sfuTrackPublished(QStringLiteral("cam-1"),
                                        QStringLiteral("TR_cam1"));
        QVERIFY2(failed.count() == 1,
                 "the failed camera's own late confirmation withdrew its "
                 "notice");

        call.setCameraCidForTest(QStringLiteral("cam-2"));
        Q_EMIT client.sfuTrackPublished(QStringLiteral("cam-2"),
                                        QStringLiteral("TR_cam2"));
        QVERIFY2(failed.count() == 2,
                 "a camera that works did not withdraw the camera's notice");
        QVERIFY(failed.at(1).at(0).toString().isEmpty());

        Q_EMIT client.sfuTrackPublished(QStringLiteral("cam-2"),
                                        QStringLiteral("TR_cam2"));
        QCOMPARE(failed.count(), 2);
    }

    void aCameraThatWorksWithdrawsOnlyTheCamerasNotice()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        const auto cameraFails = [&call](const QString &cid) {
            call.setCameraCidForTest(cid);
            QVERIFY(QMetaObject::invokeMethod(
                &call, "onEnginePublishFailed", Qt::DirectConnection,
                Q_ARG(QString, cid),
                Q_ARG(QString, QStringLiteral("camera_failed"))));
        };

        cameraFails(QStringLiteral("cam-1"));
        QCOMPARE(failed.count(), 1);
        call.setCameraCidForTest(QStringLiteral("cam-2"));
        Q_EMIT client.sfuTrackPublished(QStringLiteral("mic-1"),
                                        QStringLiteral("TR_mic1"));
        QVERIFY2(failed.count() == 1,
                 "a microphone track withdrew the camera's notice");

        QVERIFY(QMetaObject::invokeMethod(&call, "onRemotePlaybackFailed",
                                          Qt::DirectConnection,
                                          Q_ARG(bool, true)));
        QCOMPARE(failed.count(), 2);
        QVERIFY(!failed.at(1).at(0).toString().isEmpty());
        Q_EMIT client.sfuTrackPublished(QStringLiteral("cam-2"),
                                        QStringLiteral("TR_cam2"));
        QVERIFY2(failed.count() == 2,
                 "a camera that works withdrew somebody else's notice");
    }

    void aCameraNoticeFromAnEarlierCallDoesNotReachANewOne()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        call.setCameraCidForTest(QStringLiteral("cam-1"));
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("cam-1")),
            Q_ARG(QString, QStringLiteral("camera_unavailable"))));
        QCOMPARE(failed.count(), 1);
        const QString cameraText = failed.at(0).at(0).toString();
        QVERIFY(!cameraText.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(&call, "onRemotePlaybackFailed",
                                          Qt::DirectConnection,
                                          Q_ARG(bool, true)));
        call.leave();

        call.setMembershipForTest(QStringLiteral("!room:y"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        Q_EMIT call.callFailed(cameraText);
        const int before = failed.count();
        call.setCameraCidForTest(QStringLiteral("cam-2"));
        Q_EMIT client.sfuTrackPublished(QStringLiteral("cam-2"),
                                        QStringLiteral("TR_cam2"));
        QVERIFY2(failed.count() == before,
                 "a camera notice from an earlier call was withdrawn by a "
                 "camera in the new call");
        call.leave();
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
            QDateTime::currentMSecsSinceEpoch() - 120000;
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        client.emitSignal(stale);

        QCOMPARE(calls.state(), CallController::State::Idle);
        QCOMPARE(started.count(), 0);
        QVERIFY(client.sent.isEmpty());
    }

    void glareSmallerCallIdSurvivesBothDirections()
    {
        {
            RecordingCallClient client;
            CallController calls;
            calls.setClient(&client);
            QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                             QStringLiteral("v=0 sdp")));
            const QString ours = calls.activeCallId();
            CallSignal theirs = freshInvite(QString());
            theirs.callId = QStringLiteral("0000-smaller");
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
        {
            RecordingCallClient client;
            CallController calls;
            calls.setClient(&client);
            QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                             QStringLiteral("v=0 sdp")));
            const QString ours = calls.activeCallId();
            CallSignal theirs = freshInvite(QString());
            theirs.callId = QStringLiteral("zzzz-larger");
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

    void aGlareLoserAnswersTheSurvivingCallAtOnce()
    {
        RecordingCallClient client;
        client.mediaCapable = true;
        FakeMediaBackend media;
        media.videoEngine = true;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QSignalSpy rang(&calls, &CallController::incomingCallStarted);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x"),
                                QStringLiteral("@peer:x"), true));
        media.deliverOffer(calls.activeCallId(), QStringLiteral("v=0 ours"));
        CallSignal theirs = freshInvite(QString());
        theirs.callId = QStringLiteral("0000-smaller");
        theirs.eventId = QStringLiteral("$theirs");
        theirs.hasVideo = true;
        client.storedDescriptions.insert(theirs.eventId,
                                         QStringLiteral("v=0 theirs"));
        client.emitSignal(theirs);
        QCOMPARE(calls.activeCallId(), theirs.callId);
        QCOMPARE(media.answerRequests, QStringList{theirs.callId});
        QVERIFY(media.prepared.last().endsWith(
            QLatin1String("video=1 camera=1")));
        QCOMPARE(rang.size(), 0);
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

    void aGroupCallNotificationDoesNotRing()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        QSignalSpy notices(&calls, &CallController::callStartedNotice);

        const CallSignal announce = rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$announce-1"),
            QStringLiteral("@peer:x"), QStringLiteral("notification"),
true);
        client.emitSignal(announce);

        QCOMPARE(calls.state(), CallController::State::Idle);
        QVERIFY(!calls.ringing());
        QVERIFY(!calls.shouldRing());
        QCOMPARE(started.size(), 0);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().at(0).toString(), QStringLiteral("!group:x"));
        QCOMPARE(notices.first().at(1).toString(),
                 QStringLiteral("$announce-1"));
        QCOMPARE(notices.first().at(2).toString(), QStringLiteral("@peer:x"));
        QVERIFY(notices.first().at(3).toLongLong() > 0);

        client.emitSignal(announce);
        QCOMPARE(notices.size(), 1);
        QVERIFY(client.sent.isEmpty());

        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$announce-2"),
            QStringLiteral("@peer:x"), QString(),true));
        QCOMPARE(calls.state(), CallController::State::Idle);
        QCOMPARE(started.size(), 0);
        QCOMPARE(notices.size(), 2);
    }

    void aRingRingsOnlyTheUsersItAddresses()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        QSignalSpy notices(&calls, &CallController::callStartedNotice);

        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$ring-other"),
            QStringLiteral("@peer:x"), QStringLiteral("ring"),
false, {QStringLiteral("@other:x")}));
        QCOMPARE(calls.state(), CallController::State::Idle);
        QCOMPARE(started.size(), 0);
        QCOMPARE(notices.size(), 1);

        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$ring-me"),
            QStringLiteral("@peer:x"), QStringLiteral("ring"),
false,
            {QStringLiteral("@other:x"), QStringLiteral("@me:x")}));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(calls.rtcRing());
        QCOMPARE(calls.activeCallId(), QStringLiteral("$ring-me"));
        QCOMPARE(started.size(), 1);
        QVERIFY(calls.shouldRing());
        QVERIFY(calls.rejectIncoming());

        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$ring-room"),
            QStringLiteral("@peer:x"), QStringLiteral("ring"),
true));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(started.size(), 2);
        QCOMPARE(notices.size(), 1);
    }

    void aCallFromTheOtherPersonInAOneToOneDmRingsWhateverItsType()
    {
        RecordingCallClient client;
        client.roomOverrides.insert(
            QStringLiteral("!dm:x"),
            directRoom(QStringLiteral("!dm:x"), {QStringLiteral("@peer:x")}));
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);

        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!dm:x"), QStringLiteral("$dm-1"),
            QStringLiteral("@peer:x"), QStringLiteral("notification"),
true));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(started.size(), 1);
    }

    void aDmThatIsNotOneToOneWithTheCallerDoesNotRingForANotification()
    {
        RecordingCallClient client;
        client.roomOverrides.insert(
            QStringLiteral("!groupdm:x"),
            directRoom(QStringLiteral("!groupdm:x"),
                       {QStringLiteral("@peer:x"), QStringLiteral("@third:x")}));
        client.roomOverrides.insert(
            QStringLiteral("!dm:x"),
            directRoom(QStringLiteral("!dm:x"), {QStringLiteral("@peer:x")}));
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        QSignalSpy notices(&calls, &CallController::callStartedNotice);

        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!groupdm:x"), QStringLiteral("$g-1"),
            QStringLiteral("@peer:x"), QStringLiteral("notification"),
true));
        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!dm:x"), QStringLiteral("$s-1"),
            QStringLiteral("@stranger:x"), QStringLiteral("notification"),
true));
        QCOMPARE(calls.state(), CallController::State::Idle);
        QCOMPARE(started.size(), 0);
        QCOMPARE(notices.size(), 2);
    }

    void ourAnnouncementRingsOnlyInAOneToOneDm()
    {
        using C = SfuCallController;
        QCOMPARE(C::announcementTypeFor(directRoom(
                     QStringLiteral("!dm:x"), {QStringLiteral("@peer:x")})),
                 QStringLiteral("ring"));
        RoomInfo targetOnly;
        targetOnly.isDirect = true;
        targetOnly.directUserId = QStringLiteral("@peer:x");
        QCOMPARE(C::announcementTypeFor(targetOnly), QStringLiteral("ring"));
        QCOMPARE(C::announcementTypeFor(directRoom(
                     QStringLiteral("!gdm:x"),
                     {QStringLiteral("@a:x"), QStringLiteral("@b:x")})),
                 QStringLiteral("notification"));
        RoomInfo directNoTarget;
        directNoTarget.isDirect = true;
        QCOMPARE(C::announcementTypeFor(directNoTarget),
                 QStringLiteral("notification"));
        RoomInfo group;
        group.id = QStringLiteral("!group:x");
        QCOMPARE(C::announcementTypeFor(group),
                 QStringLiteral("notification"));
    }

    void aDmWithAThirdMemberIsNotOneToOne()
    {
        RoomInfo dm = directRoom(QStringLiteral("!dm:x"),
                                 {QStringLiteral("@peer:x")});
        dm.activeMemberCount = 2;
        QCOMPARE(calls::oneToOneDmPeer(dm), QStringLiteral("@peer:x"));
        QCOMPARE(SfuCallController::announcementTypeFor(dm),
                 QStringLiteral("ring"));
        dm.activeMemberCount = 3;
        QVERIFY(calls::oneToOneDmPeer(dm).isEmpty());
        QCOMPARE(SfuCallController::announcementTypeFor(dm),
                 QStringLiteral("notification"));

        RecordingCallClient client;
        client.roomOverrides.insert(QStringLiteral("!dm:x"), dm);
        CallController controller;
        controller.setClient(&client);
        controller.setOwnUserId(QStringLiteral("@me:x"));
        controller.setBacklogSuppressed(false);
        QSignalSpy started(&controller, &CallController::incomingCallStarted);
        QSignalSpy notice(&controller, &CallController::callStartedNotice);
        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!dm:x"), QStringLiteral("$announce-3"),
            QStringLiteral("@peer:x"), QStringLiteral("notification"),
true));
        QCOMPARE(controller.state(), CallController::State::Idle);
        QCOMPARE(started.size(), 0);
        QCOMPARE(notice.size(), 1);
    }

    void aCallWeStartIsAnnouncedWithTheRoomsType()
    {
        const QList<QPair<QString, QString>> cases = {
            {QStringLiteral("!dm:x"), QStringLiteral("ring")},
            {QStringLiteral("!group:x"), QStringLiteral("notification")},
        };
        for (const auto &c : cases) {
            RecordingCallClient client;
            client.roomOverrides.insert(
                QStringLiteral("!dm:x"),
                directRoom(QStringLiteral("!dm:x"),
                           {QStringLiteral("@peer:x")}));
            SfuCallController call;
            call.setClient(&client);
            const quint64 op = call.beginMembershipPublishForTest(
                c.first, kReconnectFocus,true);
            QVERIFY(op != 0);
            client.answerPublish(op, true, QString());
            QCOMPARE(client.notifies.size(), 1);
            QCOMPARE(client.notifies.first().roomId, c.first);
            QCOMPARE(client.notifies.first().type, c.second);
            QCOMPARE(client.notifies.first().membershipEventId,
                     QStringLiteral("$event"));
        }
    }

    void aRingThatMentionsUsPastTheIdBoundStillRings()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        CallSignal ring = rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$ring-me"),
            QStringLiteral("@peer:x"), QStringLiteral("ring"),
false,
            {QStringLiteral("@a:x"), QStringLiteral("@b:x")});
        client.emitSignal(ring);
        QCOMPARE(calls.state(), CallController::State::Idle);
        ring.eventId = QStringLiteral("$ring-me-2");
        ring.mentionsMe = true;
        client.emitSignal(ring);
        QCOMPARE(calls.state(), CallController::State::Ringing);
    }

    void theCallStartedNoticeFollowsTheRingsGates()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        const QString room = QStringLiteral("!group:x");
        const QString peer = QStringLiteral("@peer:x");
        QVERIFY(!calls.noticeAllowed(room, peer));
        calls.setBacklogSuppressed(false);
        QVERIFY(calls.noticeAllowed(room, peer));
        bool ignored = true;
        calls.setSenderIgnoredCheck([&](const QString &) { return ignored; });
        QVERIFY(!calls.noticeAllowed(room, peer));
        ignored = false;
        bool muted = true;
        calls.setRoomMutedCheck([&](const QString &) { return muted; });
        QVERIFY(!calls.noticeAllowed(room, peer));
        muted = false;
        bool policy = false;
        calls.setRingPolicyCheck([&](const QString &) { return policy; });
        QVERIFY(!calls.noticeAllowed(room, peer));
        policy = true;
        QVERIFY(calls.noticeAllowed(room, peer));
    }

    void noNoticeForARoomWhoseCallIsAlreadyLiveHere()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setOwnUserId(QStringLiteral("@me:x"));
        calls.setBacklogSuppressed(false);
        QSignalSpy notices(&calls, &CallController::callStartedNotice);
        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$ring-1"),
            QStringLiteral("@peer:x"), QStringLiteral("ring"),
true));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!group:x"), QStringLiteral("$notify-2"),
            QStringLiteral("@other:x"), QStringLiteral("notification"),
true));
        QCOMPARE(notices.size(), 0);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        client.emitSignal(rtcAnnouncement(
            QStringLiteral("!elsewhere:x"), QStringLiteral("$notify-3"),
            QStringLiteral("@other:x"), QStringLiteral("notification"),
true));
        QCOMPARE(notices.size(), 1);
    }

    void aDeclineFromThePersonWeRangEndsOurCall()
    {
        const auto ring = [](RecordingCallClient &client,
                             SfuCallController &call, const QString &roomId) {
            client.roomOverrides.insert(
                QStringLiteral("!dm:x"),
                directRoom(QStringLiteral("!dm:x"),
                           {QStringLiteral("@peer:x")}));
            call.setClient(&client);
            const quint64 op = call.beginMembershipPublishForTest(
                roomId, kReconnectFocus,true);
            client.answerPublish(op, true, QString());
            Q_EMIT client.rtcSendFinished(client.lastNotifyOp, true, QString(),
                                          QStringLiteral("$ring"));
        };
        const auto decline = [](const QString &roomId, const QString &sender,
                                const QString &target) {
            CallSignal signal;
            signal.kind = CallSignal::Kind::RtcDecline;
            signal.roomId = roomId;
            signal.eventId = QStringLiteral("$decline");
            signal.sender = sender;
            signal.targetEventId = target;
            return signal;
        };

        {
            RecordingCallClient client;
            SfuCallController call;
            ring(client, call, QStringLiteral("!dm:x"));
            QVERIFY(call.active());
            QSignalSpy failed(&call, &SfuCallController::callFailed);
            Q_EMIT client.callSignalReceived(decline(
                QStringLiteral("!dm:x"), QStringLiteral("@third:x"),
                QStringLiteral("$ring")));
            Q_EMIT client.callSignalReceived(decline(
                QStringLiteral("!dm:x"), QStringLiteral("@peer:x"),
                QStringLiteral("$other")));
            QVERIFY(call.active());
            Q_EMIT client.callSignalReceived(decline(
                QStringLiteral("!dm:x"), QStringLiteral("@peer:x"),
                QStringLiteral("$ring")));
            QVERIFY(!call.active());
            QCOMPARE(failed.size(), 1);
            QVERIFY(!failed.first().at(0).toString().isEmpty());
        }
        {
            RecordingCallClient client;
            SfuCallController call;
            ring(client, call, QStringLiteral("!group:x"));
            QVERIFY(call.active());
            Q_EMIT client.callSignalReceived(decline(
                QStringLiteral("!group:x"), QStringLiteral("@peer:x"),
                QStringLiteral("$ring")));
            QVERIFY(call.active());
        }
    }

    void aDeclineThatOutrunsOurRingsSendAnswerStillEndsTheCall()
    {
        const auto decline = [](const QString &target) {
            CallSignal signal;
            signal.kind = CallSignal::Kind::RtcDecline;
            signal.roomId = QStringLiteral("!dm:x");
            signal.eventId = QStringLiteral("$decline");
            signal.sender = QStringLiteral("@peer:x");
            signal.targetEventId = target;
            return signal;
        };
        for (const bool matches : {true, false}) {
            RecordingCallClient client;
            client.roomOverrides.insert(
                QStringLiteral("!dm:x"),
                directRoom(QStringLiteral("!dm:x"),
                           {QStringLiteral("@peer:x")}));
            SfuCallController call;
            call.setClient(&client);
            const quint64 op = call.beginMembershipPublishForTest(
                QStringLiteral("!dm:x"), kReconnectFocus,true);
            client.answerPublish(op, true, QString());
            QVERIFY(client.lastNotifyOp != 0);
            Q_EMIT client.callSignalReceived(decline(
                matches ? QStringLiteral("$ring") : QStringLiteral("$other")));
            QVERIFY(call.active());
            Q_EMIT client.rtcSendFinished(client.lastNotifyOp, true, QString(),
                                          QStringLiteral("$ring"));
            QCOMPARE(call.active(), !matches);
        }
    }

    void ownMembershipFromAnotherDeviceEndsTheRingAsAnsweredElsewhere()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QSignalSpy ended(&calls, &CallController::incomingCallEnded);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        calls.noteRtcSession(QStringLiteral("!other:x"), now, 2);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        calls.noteRtcSession(QStringLiteral("!r:x"), now, 2);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(),
                 CallController::EndReason::AnsweredElsewhere);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.first().at(3).toBool(), false);
        QVERIFY2(client.sent.isEmpty(),
                 "a ring answered elsewhere must not send a decline");
    }

    void anEmptiedSessionEndsTheRingAsMissed()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QSignalSpy ended(&calls, &CallController::incomingCallEnded);

        calls.noteRtcSession(QStringLiteral("!r:x"), 0, 0);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        calls.noteRtcSession(QStringLiteral("!r:x"), 0, 1);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        calls.noteRtcSession(QStringLiteral("!r:x"), 0, 0);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::RemoteHangup);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.first().at(3).toBool(), true);
    }

    void aLegacyRingIgnoresRtcSessionReports()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        calls.noteRtcSession(QStringLiteral("!r:x"),
                             QDateTime::currentMSecsSinceEpoch(), 2);
        calls.noteRtcSession(QStringLiteral("!r:x"), 0, 1);
        calls.noteRtcSession(QStringLiteral("!r:x"), 0, 0);
        QCOMPARE(calls.state(), CallController::State::Ringing);
    }

    void aStaleOwnMembershipDoesNotSilenceARealRing()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        calls.noteRtcSession(QStringLiteral("!r:x"), now - 3600 * 1000, 2);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        calls.noteRtcSession(
            QStringLiteral("!r:x"),
            now - CallController::kOwnMembershipSkewMs - 60 * 1000, 2);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        calls.noteRtcSession(QStringLiteral("!r:x"), now, 2);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(),
                 CallController::EndReason::AnsweredElsewhere);
    }

    void aSessionReadThatCannotSayWhoIsThereEndsNothing()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        calls.noteRtcSession(QStringLiteral("!r:x"), 0, 1);
        calls.noteRtcSession(QStringLiteral("!r:x"), 0, -1);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        calls.noteRtcSession(QStringLiteral("!r:x"), 0, 0);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::RemoteHangup);
    }

    void theAccountsRingPolicyKeepsTheRingSilent()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setBacklogSuppressed(false);
        QString askedFor;
        bool allowed = false;
        calls.setRingPolicyCheck([&](const QString &roomId) {
            askedFor = roomId;
            return allowed;
        });
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(!calls.shouldRing());
        QCOMPARE(askedFor, QStringLiteral("!r:x"));
        allowed = true;
        QVERIFY(calls.shouldRing());
    }

    void loweringTheBacklogGateAnnouncesARingStillLive()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(started.size(), 1);
        QVERIFY(!calls.shouldRing());
        const qint64 firstRemaining = started.first().at(3).toLongLong();

        calls.setBacklogSuppressed(false);
        QCOMPARE(started.size(), 2);
        QCOMPARE(started.last().at(0).toString(), QStringLiteral("!r:x"));
        QCOMPARE(started.last().at(1).toString(), QStringLiteral("$notify-1"));
        QCOMPARE(started.last().at(2).toString(), QStringLiteral("@peer:x"));
        const qint64 remaining = started.last().at(3).toLongLong();
        QVERIFY(remaining > 0);
        QVERIFY(remaining <= firstRemaining);
        QVERIFY(calls.shouldRing());

        calls.setBacklogSuppressed(true);
        calls.setBacklogSuppressed(false);
        QCOMPARE(started.size(), 2);
    }

    void aRingAnnouncedWithTheGateOpenIsNotAnnouncedAgain()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        calls.setBacklogSuppressed(false);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(started.size(), 1);
        calls.setBacklogSuppressed(true);
        calls.setBacklogSuppressed(false);
        QCOMPARE(started.size(), 1);
    }

    void aLegacyRingFromTheBacklogIsAnnouncedWhenTheGateLowers()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(started.size(), 1);
        calls.setBacklogSuppressed(false);
        QCOMPARE(started.size(), 2);
        QCOMPARE(started.last().at(1).toString(), QStringLiteral("call-1"));
    }

    void anEndedRingIsNotAnnouncedWhenTheGateLowers()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        QSignalSpy started(&calls, &CallController::incomingCallStarted);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QVERIFY(calls.rejectIncoming());
        calls.setBacklogSuppressed(false);
        QCOMPARE(started.size(), 1);
    }

    void rtcNotificationRingsAndDeclineUsesRtcLane()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);

        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.notificationType = QStringLiteral("ring");
        notify.mentionsRoom = true;
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
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);

        CallSignal notify;
        notify.kind = CallSignal::Kind::RtcNotification;
        notify.notificationType = QStringLiteral("ring");
        notify.mentionsRoom = true;
        notify.roomId = QStringLiteral("!r:x");
        notify.eventId = QStringLiteral("$notify-1");
        notify.sender = QStringLiteral("@peer:x");
        notify.lifetimeMs = 30000;
        notify.senderTs = QDateTime::currentMSecsSinceEpoch();
        notify.originServerTs = notify.senderTs;
        client.emitSignal(notify);
        QCOMPARE(calls.state(), CallController::State::Ringing);

        CallSignal legacy = freshInvite(QStringLiteral("legacy-1"));
        legacy.roomId = QStringLiteral("!r:x");
        legacy.sender = QStringLiteral("@peer:x");
        client.emitSignal(legacy);

        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeCallId(), QStringLiteral("$notify-1"));
        QVERIFY2(client.sent.isEmpty(),
                 "a second lane for the same call must not be rejected");
    }

    void aDifferentSenderInTheSameRoomIsStillRejectedAsBusy()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        CallSignal other = freshInvite(QStringLiteral("other-1"));
        other.roomId = QStringLiteral("!r:x");
        other.sender = QStringLiteral("@stranger:x");
        client.emitSignal(other);

        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("reject"));
    }

    void theSameSenderInADifferentRoomIsStillRejectedAsBusy()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        ringForRtcNotification(client, QStringLiteral("!r:x"),
                               QStringLiteral("@peer:x"));
        QCOMPARE(calls.state(), CallController::State::Ringing);

        CallSignal other = freshInvite(QStringLiteral("other-1"));
        other.roomId = QStringLiteral("!other:x");
        other.sender = QStringLiteral("@peer:x");
        client.emitSignal(other);

        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("reject"));
    }

    void muteStopsPublishingAndDeafenRestoresThePriorMicState()
    {
        RecordingCallClient client;
        CallController calls;
        MuteTrackingBackend backend;
        calls.setClient(&client);
        calls.setMediaBackend(&backend);
        QVERIFY(calls.muteControlAvailable());

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
        RecordingCallClient client;
        CallController calls;
        MuteTrackingBackend backend;
        calls.setClient(&client);
        calls.setMediaBackend(&backend);

        calls.setMicrophoneMuted(true);
        calls.setDeafened(true);
        backend.micCalls.clear();
        backend.outputCalls.clear();
        backend.micMuted = false;
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
        backend.micMuted = false;

        QVERIFY(calls.answer());
        QVERIFY2(backend.micMuted,
                 "answering must not publish a muted user live");
    }

    void signingOutClearsTheAudioIntent()
    {
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
        notify.notificationType = QStringLiteral("ring");
        notify.mentionsRoom = true;
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
        client.emitSendFinished(client.opCounter, false,
                                QStringLiteral("forbidden"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::SendFailed);
    }

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
        QCOMPARE(rejects, 8);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeCallId(), QStringLiteral("call-live"));
    }

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
        notify.notificationType = QStringLiteral("ring");
        notify.mentionsRoom = true;
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
        client.emitSendFinished(firstInviteOp, false,
                                QStringLiteral("network"));
        QCOMPARE(calls.state(), CallController::State::Inviting);
        QCOMPARE(failed.count(), 0);
    }

    void targetedInviteFilterUsesClientIdentity()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:x");
        CallController calls;
        calls.setClient(&client);
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
        QVERIFY(client.mediaCapable);

        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        QCOMPARE(calls.state(), CallController::State::Inviting);
        QCOMPARE(media.offerRequests.size(), 1);
        QVERIFY(client.sent.isEmpty());

        const QString callId = calls.activeCallId();
        media.deliverOffer(callId, QStringLiteral("v=0 offer"));
        QCOMPARE(client.sent.size(), 1);
        QCOMPARE(client.sent.first().kind, QStringLiteral("invite"));

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

    void aRoutedAccountNeitherPlacesNorAnswersACall()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        client.routed = true;

        QVERIFY(!calls.placeCall(QStringLiteral("!r:x")));
        QCOMPARE(calls.lastRefusal(), QStringLiteral("network_route"));
        QVERIFY(!calls.placeCallWithOffer(QStringLiteral("!r:x"), QStringLiteral("v=0 offer"),
                                          60000, QString()));
        QCOMPARE(calls.lastRefusal(), QStringLiteral("network_route"));
        QCOMPARE(calls.state(), CallController::State::Idle);

        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        client.storedDescriptions.insert(invite.eventId, QStringLiteral("v=0 remote-offer"));
        client.emitSignal(invite);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(!calls.answer());
        QCOMPARE(calls.lastRefusal(), QStringLiteral("network_route"));
        QVERIFY(media.answerRequests.isEmpty());
        QVERIFY(client.sent.isEmpty());

        client.routed = false;
        QVERIFY(calls.answer());
    }

    void answerRefusesWithoutBackendOfferOrOnRtc()
    {
        RecordingCallClient client;
        CallController calls;
        calls.setClient(&client);
        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QVERIFY(!calls.answer());
        QCOMPARE(calls.lastRefusal(), QStringLiteral("no_media_backend"));

        FakeMediaBackend media;
        calls.setMediaBackend(&media);
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
        notify.notificationType = QStringLiteral("ring");
        notify.mentionsRoom = true;
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

    void theMediaFailureNoticeNamesWhatHappened()
    {
        const auto text = [](const char *category, bool active) {
            return CallController::mediaFailureNotice(
                QString::fromLatin1(category), active);
        };
        const QString setup = text("media_pipeline", false);
        const QString stopped = text("media_pipeline", true);
        const QString lost = text("media_connection", true);
        const QString cannotConnect = text("media_connection", false);
        QCOMPARE(text("media_receive", true), lost);
        QCOMPARE(text("connection_timeout", false), cannotConnect);
        QVERIFY(setup != stopped && setup != lost && setup != cannotConnect);
        QVERIFY(stopped != lost && lost != cannotConnect);
        QVERIFY2(!lost.contains(QLatin1String("set up")), qPrintable(lost));
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

        QSignalSpy ended(&calls, &CallController::mediaFailureEnded);
        media.deliverFailure(callId, QStringLiteral("device"));
        QCOMPARE(ended.count(), 1);
        QCOMPARE(ended.first().at(0).toString(), QStringLiteral("device"));
        QCOMPARE(ended.first().at(1).toBool(), false);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::MediaFailed);
        QVERIFY(client.sent.isEmpty());
        QCOMPARE(media.closed, QStringList{callId});

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
        media.deliverOffer(QStringLiteral("stale-call"),
                           QStringLiteral("v=0 stale"));
        QVERIFY(client.sent.isEmpty());
        QCOMPARE(calls.state(), CallController::State::Inviting);
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
        QVERIFY(QMetaObject::invokeMethod(&calls, "onLifetimeExpired"));
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::MediaFailed);
        QVERIFY(client.sent.isEmpty());
    }

    void sdpStoreIsBoundedSingleShotAndClearable()
    {
        calls::SdpStore store;
        store.insert(QStringLiteral("$e1"), QStringLiteral("v=0 one"));
        QCOMPARE(store.take(QStringLiteral("$e1")), QStringLiteral("v=0 one"));
        QCOMPARE(store.take(QStringLiteral("$e1")), QString());
        store.insert(QString(), QStringLiteral("x"));
        store.insert(QStringLiteral("$e"), QString());
        QCOMPARE(store.size(), 0);
        for (int i = 0; i < calls::SdpStore::kCapacity + 2; ++i)
            store.insert(QStringLiteral("$evt-%1").arg(i),
                         QStringLiteral("sdp-%1").arg(i));
        QCOMPARE(store.size(), calls::SdpStore::kCapacity);
        QCOMPARE(store.take(QStringLiteral("$evt-0")), QString());
        QCOMPARE(store.take(QStringLiteral("$evt-1")), QString());
        QVERIFY(!store.take(QStringLiteral("$evt-2")).isEmpty());
        store.clear();
        store.insert(QStringLiteral("$dup"), QStringLiteral("a"));
        store.insert(QStringLiteral("$dup"), QStringLiteral("b"));
        QCOMPARE(store.size(), 1);
        QCOMPARE(store.take(QStringLiteral("$dup")), QStringLiteral("b"));
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
        QVERIFY(!calls.hangup());
        QVERIFY(calls.answer());
        media.deliverAnswer(QStringLiteral("call-1"),
                            QStringLiteral("v=0 answer"));
        QCOMPARE(calls.state(), CallController::State::Connecting);
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
        QCOMPARE(ended.at(0).at(3).toBool(), false);

        client.emitSignal(freshInvite(QStringLiteral("call-2")));
        CallSignal hangup2 = hangup;
        hangup2.eventId = QStringLiteral("$hangup-2");
        hangup2.callId = QStringLiteral("call-2");
        client.emitSignal(hangup2);
        QCOMPARE(ended.count(), 2);
        QCOMPARE(ended.at(1).at(3).toBool(), true);
    }

    void offerProductionPhaseNeverTouchesTheWire()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x")));
        QVERIFY(calls.hangup());
        QCOMPARE(calls.state(), CallController::State::Ended);
        QVERIFY(client.sent.isEmpty());

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
        QCOMPARE(client.candidateBatches.size(), 0);
        QTest::qWait(250);
        QCOMPARE(client.candidateBatches.size(), 1);
        QCOMPARE(client.candidateBatches.first().second.size(), 2);

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
        QVERIFY(calls.placeCall(QStringLiteral("!r:x"), QStringLiteral("@peer:x")));
        const QString callId = calls.activeCallId();
        media.deliverOffer(callId, QStringLiteral("v=0 offer"));
        QVariantList batch;
        QVariantMap entry;
        entry.insert(QStringLiteral("candidate"),
                     QStringLiteral("candidate:0 1 UDP x"));
        batch.append(entry);
        client.emitCandidates(QStringLiteral("!r:x"),
                              QStringLiteral("other"), false, batch);
        QVERIFY(media.remoteCandidates.isEmpty());
        client.emitCandidates(QStringLiteral("!r:x"), callId, true, batch);
        QVERIFY(media.remoteCandidates.isEmpty());
        client.emitCandidates(QStringLiteral("!r:x"), callId, false, batch);
        QCOMPARE(media.remoteCandidates.size(), 1);
    }

    void firstOfferWaitsForTurnBeforeCreatingTheMediaSession()
    {
        RecordingCallClient client;
        client.deferTurn = true;
        FakeMediaBackend media;
        CallController calls;
        calls.setMediaBackend(&media);
        calls.setClient(&client);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x"),
                                QStringLiteral("@peer:x")));
        QVERIFY(media.offerRequests.isEmpty());
        client.emitTurnServers(client.lastTurnOp, true,
                               {QStringLiteral("turn:relay.example:3478")});
        QCOMPARE(media.offerRequests.size(), 1);
        QCOMPARE(media.iceServerApplications.last(),
                 QStringList{QStringLiteral("turn:relay.example:3478")});
    }

    void firstAnswerWaitsForTurnAndRetainsEarlyCandidates()
    {
        RecordingCallClient client;
        client.deferTurn = true;
        FakeMediaBackend media;
        CallController calls;
        calls.setMediaBackend(&media);
        calls.setClient(&client);
        const auto invite = freshInvite(QStringLiteral("call-turn"));
        client.storedDescriptions.insert(invite.eventId, QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QVariantMap candidate;
        candidate.insert(QStringLiteral("candidate"), QStringLiteral("candidate:1 1 UDP x"));
        client.emitCandidates(invite.roomId, invite.callId, false, {candidate});
        QVERIFY(calls.answer());
        QVERIFY(media.answerRequests.isEmpty());
        QVERIFY(media.remoteCandidates.isEmpty());
        client.emitCandidates(invite.roomId, invite.callId, false, {candidate});
        client.emitTurnServers(client.lastTurnOp, true,
                               {QStringLiteral("turn:relay.example:3478")});
        QCOMPARE(media.answerRequests.size(), 1);
        QCOMPARE(media.remoteCandidates.size(), 2);
    }

    void changingAccountsCancelsPendingMediaAndDropsThePreviousTurnCredentials()
    {
        RecordingCallClient first;
        RecordingCallClient second;
        first.deferTurn = second.deferTurn = true;
        FakeMediaBackend media;
        CallController calls;
        calls.setMediaBackend(&media);
        calls.setClient(&first);
        QVERIFY(calls.placeCall(QStringLiteral("!first:x"), QStringLiteral("@peer:x")));
        QVERIFY(media.offerRequests.isEmpty());
        calls.setClient(&second);
        QCOMPARE(calls.state(), CallController::State::Ended);
        QCOMPARE(calls.endReason(), CallController::EndReason::SessionLost);
        QVERIFY(calls.placeCall(QStringLiteral("!second:x"), QStringLiteral("@peer:x")));
        first.emitTurnServers(first.lastTurnOp, true, {QStringLiteral("turn:old.example:3478")});
        QVERIFY(media.offerRequests.isEmpty());
        second.emitTurnServers(second.lastTurnOp, true, {QStringLiteral("turn:new.example:3478")});
        QCOMPARE(media.offerRequests.size(), 1);
        QCOMPARE(media.iceServerApplications.last(), QStringList{QStringLiteral("turn:new.example:3478")});
    }

    void anEndedCallCannotStartMediaFromALateTurnResponse()
    {
        RecordingCallClient client;
        client.deferTurn = true;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x"), QStringLiteral("@peer:x")));
        QVERIFY(media.offerRequests.isEmpty());
        calls.hangup();
        client.emitTurnServers(client.lastTurnOp, true,
                               {QStringLiteral("turn:relay.example:3478")});
        QVERIFY(media.offerRequests.isEmpty());
        QCOMPARE(calls.state(), CallController::State::Ended);
    }

    void aTurnTimeoutStartsDirectMediaOnceAndIgnoresTheLateReply()
    {
        RecordingCallClient client;
        client.deferTurn = true;
        FakeMediaBackend media;
        CallController calls;
        calls.setTurnWaitTimeoutForTest(30);
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        QVERIFY(calls.placeCall(QStringLiteral("!r:x"), QStringLiteral("@peer:x")));
        QVERIFY(media.offerRequests.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(media.offerRequests.size(), 1, 1000);
        client.emitTurnServers(client.lastTurnOp, true,
                               {QStringLiteral("turn:relay.example:3478")});
        QCOMPARE(media.offerRequests.size(), 1);
    }

    void anAcceptedCallHasABoundedMediaConnectionDeadline()
    {
        for (bool inbound : {false, true}) {
            RecordingCallClient client;
            FakeMediaBackend media;
            CallController calls;
            calls.setClient(&client);
            calls.setMediaBackend(&media);
            calls.setMediaConnectionTimeoutForTest(30);
            if (inbound) {
                const auto invite = freshInvite(QStringLiteral("call-connect"));
                client.storedDescriptions.insert(invite.eventId, QStringLiteral("v=0 offer"));
                client.emitSignal(invite);
                QVERIFY(calls.answer());
                media.deliverAnswer(invite.callId, QStringLiteral("v=0 answer"));
            } else {
                QVERIFY(calls.placeCall(QStringLiteral("!r:x"), QStringLiteral("@peer:x")));
                const auto id = calls.activeCallId();
                media.deliverOffer(id, QStringLiteral("v=0 offer"));
                auto answer = freshInvite(id);
                answer.kind = CallSignal::Kind::Answer;
                client.storedDescriptions.insert(answer.eventId, QStringLiteral("v=0 answer"));
                client.emitSignal(answer);
            }
            QCOMPARE(calls.state(), CallController::State::Connecting);
            const auto id = calls.activeCallId();
            QTRY_COMPARE_WITH_TIMEOUT(calls.state(), CallController::State::Ended, 1000);
            QCOMPARE(calls.endReason(), CallController::EndReason::MediaFailed);
            media.deliverConnected(id);
            QCOMPARE(calls.state(), CallController::State::Ended);
        }
    }

    void remoteIceCandidatesMustCarryTheBoundSender()
    {
        RecordingCallClient client;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        const auto invite = freshInvite(QStringLiteral("call-sender"));
        client.storedDescriptions.insert(invite.eventId, QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QVariantMap candidate;
        candidate.insert(QStringLiteral("candidate"), QStringLiteral("candidate:1 1 UDP x"));
        const QVariantList batch{candidate};
        auto emitFrom = [&](const QString &sender) {
            return QMetaObject::invokeMethod(&client, "callCandidatesReceived", Qt::DirectConnection,
                Q_ARG(QString, invite.roomId), Q_ARG(QString, invite.callId),
                Q_ARG(QString, invite.partyId), Q_ARG(QString, sender),
                Q_ARG(bool, false), Q_ARG(QVariantList, batch));
        };
        QVERIFY(emitFrom(QStringLiteral("@mallory:x")));
        QVERIFY(calls.answer());
        QVERIFY(media.remoteCandidates.isEmpty());
        QVERIFY(emitFrom(invite.sender));
        QCOMPARE(media.remoteCandidates.size(), 1);
        QVERIFY(emitFrom(QStringLiteral("@mallory:x")));
        QCOMPARE(media.remoteCandidates.size(), 1);
    }

    void outgoingEarlyCandidatesBelongToTheInvitedUserEvenBeforeAnswer()
    {
        RecordingCallClient client;
        client.deferTurn = true;
        FakeMediaBackend media;
        CallController calls;
        calls.setClient(&client);
        calls.setMediaBackend(&media);
        const QString room = QStringLiteral("!r:x");
        QVERIFY(calls.placeCall(room, QStringLiteral("@peer:x")));
        const QString id = calls.activeCallId();
        const QVariantList candidates{QVariantMap{
            {QStringLiteral("candidate"), QStringLiteral("candidate:1 1 UDP x")}}};
        client.emitCandidates(room, id, false, candidates, QStringLiteral("@mallory:x"));
        client.emitCandidates(room, id, false, candidates);
        QVERIFY(media.remoteCandidates.isEmpty());
        client.emitTurnServers(client.lastTurnOp, true, {QStringLiteral("turn:relay.example:3478")});
        QCOMPARE(media.remoteCandidates.size(), 1);
        client.emitCandidates(room, id, false, candidates, QStringLiteral("@mallory:x"));
        QCOMPARE(media.remoteCandidates.size(), 1);
        client.emitCandidates(room, id, false, candidates);
        QCOMPARE(media.remoteCandidates.size(), 2);
    }

    void turnServersFlowToTheEngineOnceFetched()
    {
        RecordingCallClient client;
        client.deferTurn = true;
        FakeMediaBackend media;
        CallController calls;
        calls.setMediaBackend(&media);
        QCOMPARE(client.lastTurnOp, quint64(0));
        calls.setClient(&client);
        QVERIFY(client.lastTurnOp != 0);
        client.emitTurnServers(client.lastTurnOp + 999, true,
                               { QStringLiteral("turn:one") });
        QVERIFY(media.iceServerApplications.isEmpty());
        client.emitTurnServers(client.lastTurnOp, true,
                               { QStringLiteral("turn:one"),
                                 QStringLiteral("stun:two") });
        QCOMPARE(media.iceServerApplications.size(), 1);
        QCOMPARE(media.iceServerApplications.first().size(), 2);
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
        QCOMPARE(total, 41);
    }

    void turnResponseIsBoundedDefensively()
    {
        RecordingCallClient client;
        client.deferTurn = true;
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
        QCOMPARE(media.iceServerApplications.first().size(), 16);
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

        client.emitSignal(freshInvite(QStringLiteral("call-1")));
        QCOMPARE(calls.state(), CallController::State::Ringing);
    }

    void speakerUpdatesNeverResetTheParticipantModel()
    {
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
        QVERIFY(changes.count() > 0);
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
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
        });
        call.ingestSpeakersForTest(
            { speakerEntry(QStringLiteral("PA_1"), true) });

        CallParticipantModel *model = call.participantModel();
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::SpeakingRole).toBool(),
                 true);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::SpeakingLevelRole)
                     .toDouble(),
                 0.0);
    }

    void speakersChangedIsADeltaSoASteadySpeakerKeepsItsRing()
    {
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"), {}),
        });
        CallParticipantModel *model = call.participantModel();
        const int alice = participantRowFor(model, QStringLiteral("alice"));
        const int bob = participantRowFor(model, QStringLiteral("bob"));
        const auto speaking = [&](int row) {
            return participantRole(model, row, CallParticipantModel::SpeakingRole)
                .toBool();
        };
        call.ingestSpeakersForTest(
            { speakerEntry(QStringLiteral("PA_1"), true, 0.5) });
        call.ingestSpeakersForTest(
            { speakerEntry(QStringLiteral("PA_2"), true, 0.4) });
        QVERIFY(speaking(alice));
        QVERIFY(speaking(bob));
        call.ingestSpeakersForTest({});
        QVERIFY(speaking(alice));
        QVERIFY(speaking(bob));
        call.ingestSpeakersForTest(
            { speakerEntry(QStringLiteral("PA_1"), false, 0.0) });
        QVERIFY(!speaking(alice));
        QCOMPARE(participantRole(model, alice,
                                 CallParticipantModel::SpeakingLevelRole)
                     .toDouble(),
                 0.0);
        QVERIFY(speaking(bob));
        QVariantMap gone = sfuParticipant(QStringLiteral("bob"),
                                          QStringLiteral("PA_2"), {});
        gone.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
        call.ingestParticipantsForTest({ gone });
        QCOMPARE(model->rowCount(), 1);
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           {}),
            sfuParticipant(QStringLiteral("bob"), QStringLiteral("PA_2"), {}),
        });
        QVERIFY(!speaking(participantRowFor(model, QStringLiteral("bob"))));
    }

    void aMuteChangeIsOneRoleOnOneRowNotAMembershipChange()
    {
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
        QCOMPARE(shares->get(0).value(QStringLiteral("trackKey")).toString(),
                 QStringLiteral("TR_share_a"));
        QCOMPARE(shares->get(1).value(QStringLiteral("trackKey")).toString(),
                 QStringLiteral("TR_share_b"));
    }

    void aDismissedShareStaysLiveAndIsAlwaysReachableAgain()
    {
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
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_b"));

        stage->dismissShare(QStringLiteral("TR_share_b"));
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_a"));
        QCOMPARE(shares->rowCount(), 2);

        stage->dismissShare(QStringLiteral("TR_share_a"));
        QCOMPARE(stage->spotlightShareId(), QString());
        QCOMPARE(shares->rowCount(), 2);
        QCOMPARE(stage->restorableShareAvailable(), true);
        QCOMPARE(stage->dismissedShareCount(), 2);

        stage->restoreAllShares();
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_b"));
        QCOMPARE(stage->restorableShareAvailable(), false);
    }

    void aNewShareReArmsTheSpotlightAfterTheUserChoseGrid()
    {
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
        SfuCallController call;
        CallStageState *stage = call.stageState();
        stage->setLayoutPreference(QStringLiteral("spotlight"));
        stage->setLayoutPreference(QStringLiteral("nonsense"));
        QCOMPARE(stage->layoutPreference(), QStringLiteral("spotlight"));
    }

    void aRestartedShareIsOfferedAgainRatherThanInheritingADismissal()
    {
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_1"), false) }),
        });
        CallStageState *stage = call.stageState();
        stage->dismissShare(QStringLiteral("TR_share_1"));
        QCOMPARE(stage->spotlightShareId(), QString());

        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_1"), true) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 0);
        QCOMPARE(stage->dismissedShareCount(), 0);

        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_2"), false) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 1);
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_2"));
        QCOMPARE(stage->isShareDismissed(QStringLiteral("TR_share_2")), false);
    }

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
        call.setLocalMediaStateForTest(false,
true);
        QCOMPARE(call.shareModel()->rowCount(), 1);

        call.setLocalMediaStateForTest(false,
false);
        QCOMPARE(call.shareModel()->rowCount(), 0);
    }

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
        call.setLocalMediaStateForTest(false, true);
        QCOMPARE(client.muteRequests.size(), 0);

        call.setLocalMediaStateForTest(false, false);
        QCOMPARE(client.muteRequests.size(), 1);
        QCOMPARE(client.muteRequests.at(0).first, QStringLiteral("TR_mine"));
        QCOMPARE(client.muteRequests.at(0).second, true);

        call.setLocalMediaStateForTest(false, false);
        QCOMPARE(client.muteRequests.size(), 2);
        QCOMPARE(client.muteRequests.at(1).first, QStringLiteral("TR_mine"));
        QCOMPARE(client.muteRequests.at(1).second, true);

        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_mine"), true) }),
        });
        client.muteRequests.clear();
        call.setLocalMediaStateForTest(false, false);
        QCOMPARE(client.muteRequests.size(), 0);
    }

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
        call.setLocalMediaStateForTest(true, false);
        CallParticipantModel *model = call.participantModel();
        const int mine = participantRowFor(model, QStringLiteral("me"));
        QVERIFY(mine >= 0);
        QCOMPARE(participantRole(model, mine,
                                 CallParticipantModel::CameraOnRole).toBool(),
                 true);

        call.setLocalMediaStateForTest(false, false);
        QCOMPARE(participantRole(model, mine,
                                 CallParticipantModel::CameraOnRole).toBool(),
                 false);
        QCOMPARE(client.muteRequests.size(), 1);
        QCOMPARE(client.muteRequests.at(0).first, QStringLiteral("TR_cam"));
        QCOMPARE(client.muteRequests.at(0).second, true);
    }

    void anSfuIdentityThatResolvesToNobodySaysSoExactlyOnce()
    {
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

        {
            Capture capture(&lines);
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
            QVERIFY(rtc.participantForIdentity(room, identity).isEmpty());
        }
        QVERIFY2(countSaid() == 0,
                 "a transient lookup miss must not be reported as a "
                 "permanent one");

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

        lines.clear();
        rtc.forgetUnresolvedIdentityDiagnostics();
        {
            Capture capture(&lines);
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

        call.setCallStateForTest(SfuCallController::State::Idle);
        const int idleBefore = call.keyLaneReconcilesForTest();
        QMetaObject::invokeMethod(&call, "reconcileKeyLane");
        QCOMPARE(call.keyLaneReconcilesForTest(), idleBefore);

        call.setCallStateForTest(SfuCallController::State::Connected);
        QMetaObject::invokeMethod(&call, "reconcileKeyLane");
        QCOMPARE(call.keyLaneReconcilesForTest(), idleBefore + 1);

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

        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_BEA"), {}),
        });
        CallParticipantModel *model = call.participantModel();
        QCOMPARE(participantRowFor(model, identity), -1);

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

    void aRestartedLocalShareNeverUnmutesTheTrackItJustStopped()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setOwnIdentityForTest(QStringLiteral("me"));
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("me"), QStringLiteral("PA_ME"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_dead"), true) }),
        });
        call.setLocalMediaStateForTest(false,
true);
        QCOMPARE(client.muteRequests.size(), 0);

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
        call.setParticipantVolume(QStringLiteral("alice"), 400);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);
        call.setParticipantVolume(QStringLiteral("alice"), 200);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 200);
        call.setParticipantVolume(QStringLiteral("alice"), -25);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 0);
    }

    void connectionQualityIsMergedAndUnknownIsNeverRendered()
    {
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
        QCOMPARE(participantRole(model, 1,
                                 CallParticipantModel::ConnectionQualityRole)
                     .toString(),
                 QString());

        call.ingestConnectionQualityForTest({ unknown });
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::ConnectionQualityRole)
                     .toString(),
                 QStringLiteral("excellent"));
    }

    void leavingClearsTheStageStateSoTheNextCallStartsClean()
    {
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

    void tracklessUnmatchedSfuConnectionsDoNotBecomePeople()
    {
        RecordingCallClient client;
        RtcController rtc;
        SfuCallController call;
        const QString room = QStringLiteral("!room:x");
        const QString identity = QStringLiteral("@peer:x:DEV");
        stageOneRemoteParticipant(client, rtc, call, room, identity,
                                  QStringLiteral("@peer:x"), QStringLiteral("DEV"),
                                  QStringLiteral("$member"));
        const int before = call.participantCount();
        const QString observer = QStringLiteral("unmatched-observer-hash");
        call.ingestParticipantsForTest({sfuParticipant(observer, QStringLiteral("PA_OBSERVER"), {})});
        QCOMPARE(call.participantCount(), before);
        RtcSessionData session;
        session.roomId = room;
        RtcParticipant peer;
        peer.userId = QStringLiteral("@peer:x");
        peer.deviceId = QStringLiteral("DEV");
        peer.rtcIdentity = identity;
        RtcParticipant late;
        late.userId = QStringLiteral("@late:x");
        late.deviceId = QStringLiteral("LATE");
        late.rtcIdentity = observer;
        session.participants = {peer, late};
        rtc.refresh(room);
        client.answerSession(client.lastSessionOp, session);
        QCOMPARE(call.participantCount(), before + 1);
    }

    void participantsInvokableIsReadOutOfTheModel()
    {
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
        SfuCallController call;
        CallStageState *stage = call.stageState();
        QVERIFY(stage);
        QCOMPARE(stage->fullScreen(), false);

        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), false);

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

        stage->setFullScreen(false);
        stage->dismissShare(QStringLiteral("TR_share_a"));
        QCOMPARE(stage->spotlightShareId(), QString());
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), false);
        stage->pin(QStringLiteral("alice"));
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
    }

    void pictureInPictureAndFullScreenAreMutuallyExclusive()
    {
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

        stage->setPictureInPicture(true);
        QCOMPARE(pipSpy.count(), 1);
    }

    void aCallEndingTakesTheFloatingWindowWithIt()
    {
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

    void pictureInPictureIsOfferedOnAVoiceOnlyCall()
    {
        SfuCallController call;
        CallStageState *stage = call.stageState();
        QVERIFY(stage);
        QCOMPARE(stage->spotlightShareId(), QString());
        QCOMPARE(stage->pinnedIdentity(), QString());
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), false);
        stage->setPictureInPicture(true);
        QCOMPARE(stage->pictureInPicture(), true);
    }

    void fullScreenDropsItselfWhenTheFocusedSurfaceGoesAway()
    {
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        CallStageState *stage = call.stageState();
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);

        QVariantMap stopped = sfuParticipant(QStringLiteral("alice"),
                                             QStringLiteral("PA_1"),
                                             { sfuTrack(
                                                 QStringLiteral("screen_share"),
                                                 QStringLiteral("TR_share_a"),
                                                 true) });
        call.ingestParticipantsForTest({ stopped });
        QCOMPARE(stage->spotlightShareId(), QString());
        QCOMPARE(stage->fullScreen(), false);

        stage->pin(QStringLiteral("alice"));
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
        stage->clearPin();
        QCOMPARE(stage->fullScreen(), false);

        stage->pin(QStringLiteral("alice"));
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
        call.leave();
        QCOMPARE(stage->fullScreen(), false);
    }

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

    void aFailedRetractionIsRetriedWithBackoff()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QStringLiteral("delay-1"));
        call.leave();
        QCOMPARE(client.retractions.size(), 1);

        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QTRY_COMPARE_WITH_TIMEOUT(client.retractions.size(), 2, 5000);
        QCOMPARE(client.retractions.at(1).first,
                 QStringLiteral("!room:example.org"));
        QCOMPARE(client.retractions.at(1).second, QStringLiteral("delay-1"));

        for (int i = 0; i < 8; ++i) {
            client.answerMembershipOp(client.lastRetractOp, false,
                                      QStringLiteral("network"));
            QTest::qWait(120);
        }
        QCOMPARE(client.retractions.size(), 2);
        QVERIFY(call.membershipWritesPending());
    }

    void aRetractionStaysOwedThroughANetworkOutageAndIsSentOnceItEnds()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        call.leave();
        QCOMPARE(client.retractions.size(), 1);

        for (int i = 0; i < 6; ++i) {
            client.answerMembershipOp(client.lastRetractOp, false,
                                      QStringLiteral("network"));
            QVERIFY2(call.membershipWritesPending(),
                     "a retraction that failed for network reasons was "
                     "dropped while the membership is still live");
            QVERIFY(QMetaObject::invokeMethod(&call, "retryRetraction",
                                              Qt::DirectConnection));
            QCOMPARE(client.retractions.size(), i + 2);
            QCOMPARE(client.retractions.last().first,
                     QStringLiteral("!room:example.org"));
        }

        client.answerMembershipOp(client.lastRetractOp, true, QString());
        QVERIFY(!call.membershipWritesPending());
        const int sent = client.retractions.size();

        QVERIFY(QMetaObject::invokeMethod(&call, "retryRetraction",
                                          Qt::DirectConnection));
        Q_EMIT client.homeserverReachable();
        QTest::qWait(2300);
        QCOMPARE(client.retractions.size(), sent);
    }

    void anOwedRetractionIsSentTheMomentTheHomeserverAnswersAgain()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        call.leave();
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QCOMPARE(client.retractions.size(), 1);

        Q_EMIT client.homeserverReachable();
        QCOMPARE(client.retractions.size(), 2);
        Q_EMIT client.homeserverReachable();
        QCOMPARE(client.retractions.size(), 2);

        client.answerMembershipOp(client.lastRetractOp, true, QString());
        QVERIFY(!call.membershipWritesPending());
        QTest::qWait(2300);
        Q_EMIT client.homeserverReachable();
        QCOMPARE(client.retractions.size(), 2);
    }

    void anAttemptThatFailsAfterTheHomeserverReturnedIsRetriedPromptly()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        call.leave();
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QVERIFY(QMetaObject::invokeMethod(&call, "retryRetraction",
                                          Qt::DirectConnection));
        QCOMPARE(client.retractions.size(), 2);

        Q_EMIT client.homeserverReachable();
        QCOMPARE(client.retractions.size(), 2);
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QTRY_COMPARE_WITH_TIMEOUT(client.retractions.size(), 3, 3200);
    }

    void anOwedRetractionEndsWithTheMembershipsExpiry()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setRetractionOwedForMsForTest(150);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        call.leave();
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QTRY_COMPARE_WITH_TIMEOUT(client.retractions.size(), 2, 1000);

        QTest::qWait(200);
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QVERIFY2(!call.membershipWritesPending(),
                 "a retraction is still owed past the membership's expiry");
        Q_EMIT client.homeserverReachable();
        QVERIFY(QMetaObject::invokeMethod(&call, "retryRetraction",
                                          Qt::DirectConnection));
        QCOMPARE(client.retractions.size(), 2);
    }

    void anOwedRetractionIsNeverSentUnderAnotherAccount()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@first:example.org");
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        call.leave();
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QCOMPARE(client.retractions.size(), 1);

        client.simulatedUserId = QStringLiteral("@second:example.org");
        Q_EMIT client.homeserverReachable();
        QCOMPARE(client.retractions.size(), 1);
        QVERIFY(!call.membershipWritesPending());
        QVERIFY(QMetaObject::invokeMethod(&call, "retryRetraction",
                                          Qt::DirectConnection));
        QCOMPARE(client.retractions.size(), 1);
    }

    void anOwedRetractionIsNeverSentFromAnotherDevice()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:example.org");
        client.simulatedDeviceId = QStringLiteral("DEVICEONE");
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        call.leave();
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));

        client.simulatedDeviceId = QStringLiteral("DEVICETWO");
        Q_EMIT client.homeserverReachable();
        QCOMPARE(client.retractions.size(), 1);
        QVERIFY(!call.membershipWritesPending());
    }

    void aLongExpiryWithNoDelayedLeaveKeepsTheRetractionOwedLonger()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setRetractionOwedForMsForTest(150);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.publishes.size(), 1);
        client.answerPublish(client.lastPublishOp, false, QString(),
                             QStringLiteral("network"));

        call.leave();
        QCOMPARE(client.retractions.size(), 1);
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QTest::qWait(400);
        client.answerMembershipOp(client.lastRetractOp, false,
                                  QStringLiteral("network"));
        QVERIFY2(call.membershipWritesPending(),
                 "a retraction owed to a 4 h membership was dropped after "
                 "the 5 minute window");
    }

    void aRetractionWhoseAnswerNeverArrivesCountsAsFailed()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setRetractionAnswerTimeoutMsForTest(100);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());
        call.leave();
        QCOMPARE(client.retractions.size(), 1);
        QTest::qWait(300);
        QVERIFY(call.membershipWritesPending());
        Q_EMIT client.homeserverReachable();
        QCOMPARE(client.retractions.size(), 2);
    }

    void aRetractionLandingAfterARejoinRepublishesTheMembership()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(room, QString());
        call.leave();
        QCOMPARE(client.retractions.size(), 1);
        const quint64 oldRetraction = client.lastRetractOp;

        const quint64 publish = call.beginMembershipPublishForTest(
            room, QStringLiteral("https://sfu.example.org"));
        QVERIFY(publish != 0);
        client.answerPublish(publish, true, QString());
        QVERIFY(call.active());
        QVERIFY(!call.membershipWritesPending());
        const int publishesBefore = client.publishes.size();

        client.answerMembershipOp(oldRetraction, true, QString());
        QCOMPARE(client.publishes.size(), publishesBefore + 1);
        QCOMPARE(client.publishes.last(), room);
        QCOMPARE(client.retractions.size(), 1);
        QVERIFY(!call.membershipWritesPending());
    }

    void aRetractionLandingAfterARejoinDoesNotRepublishOverADelayedLeave()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(room, QStringLiteral("delay-1"));
        call.leave();
        const quint64 oldRetraction = client.lastRetractOp;

        const quint64 publish = call.beginMembershipPublishForTest(
            room, QStringLiteral("https://sfu.example.org"));
        client.answerPublish(publish, true, QStringLiteral("delay-2"));
        QVERIFY(call.active());
        const int publishesBefore = client.publishes.size();

        client.answerMembershipOp(oldRetraction, true, QString());
        QVERIFY2(client.publishes.size() == publishesBefore,
                 "a re-publish while a delayed retraction is armed arms an "
                 "orphan that later removes the live membership");
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.delayedRestarts.last(), QStringLiteral("delay-2"));
    }

    void aRetractionLandingDuringTheRejoinPublishDoesNotRepublish()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(room, QString());
        call.leave();
        const quint64 oldRetraction = client.lastRetractOp;

        QVERIFY(call.beginMembershipPublishForTest(
                    room, QStringLiteral("https://sfu.example.org"))
                != 0);
        QVERIFY(call.active());
        const int publishesBefore = client.publishes.size();
        client.answerMembershipOp(oldRetraction, true, QString());
        QCOMPARE(client.publishes.size(), publishesBefore);
    }

    void aRefusedRetractionFromBeforeARejoinChangesNothing()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(room, QString());
        call.leave();
        const quint64 oldRetraction = client.lastRetractOp;
        const quint64 publish = call.beginMembershipPublishForTest(
            room, QStringLiteral("https://sfu.example.org"));
        client.answerPublish(publish, true, QString());
        const int publishesBefore = client.publishes.size();

        client.answerMembershipOp(oldRetraction, false,
                                  QStringLiteral("network"));
        QCOMPARE(client.publishes.size(), publishesBefore);
        QCOMPARE(client.retractions.size(), 1);
    }

    void theOwedWindowsMirrorTheRustExpiryConstants()
    {
        const auto product = [](const QByteArray &source,
                                const QByteArray &name) -> qint64 {
            const int at = source.indexOf(name);
            if (at < 0)
                return -1;
            const int eq = source.indexOf('=', at);
            const int end = source.indexOf(';', eq);
            if (eq < 0 || end < 0)
                return -1;
            qint64 value = 1;
            const QByteArray expr = source.mid(eq + 1, end - eq - 1);
            for (QByteArray factor : expr.split('*')) {
                factor = factor.trimmed();
                if (factor.endsWith("LL"))
                    factor.chop(2);
                bool ok = false;
                const qint64 n = factor.toLongLong(&ok);
                if (!ok)
                    return -1;
                value *= n;
            }
            return value;
        };
        QFile cpp(QStringLiteral(SOURCE_DIR "/src/calls/SfuCallController.cpp"));
        QVERIFY(cpp.open(QIODevice::ReadOnly));
        const QByteArray c = cpp.readAll();
        QFile rs(QStringLiteral(SOURCE_DIR "/rust/src/rtc.rs"));
        QVERIFY(rs.open(QIODevice::ReadOnly));
        const QByteArray r = rs.readAll();

        const qint64 shortCpp =
            product(c, "constexpr qint64 kMembershipExpiryNoDelayedMs");
        const qint64 shortRs =
            product(r, "const MEMBERSHIP_EXPIRY_NO_DELAYED_MS: u64");
        QVERIFY(shortCpp > 0);
        QCOMPARE(shortCpp, shortRs);
        const qint64 longCpp =
            product(c, "constexpr qint64 kMembershipExpiryLongMs");
        const qint64 longRs = product(r, "const MEMBERSHIP_EXPIRY_MS: u64");
        QVERIFY(longCpp > 0);
        QCOMPARE(longCpp, longRs);
    }

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
        QTest::qWait(2600);
        QCOMPARE(client.retractions.size(), 1);
    }

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

    void withoutADelayedRetractionTheHeartbeatRePublishesTheMembership()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setMembershipForTest(QStringLiteral("!room:example.org"),
                                  QString());

        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.publishes.size(), 1);
        QCOMPARE(client.publishes.first(), QStringLiteral("!room:example.org"));
        QCOMPARE(client.delayedRestarts.size(), 0);

        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.publishes.size(), 1);
    }

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

        client.answerPublish(client.lastPublishOp, true,
                             QStringLiteral("delay-2"));
        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        QCOMPARE(client.delayedRestarts.size(), 2);
        QCOMPARE(client.delayedRestarts.at(1), QStringLiteral("delay-2"));
    }

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
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Connected));
        QCOMPARE(call.roomId(), QStringLiteral("!room:example.org"));
    }

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

        QVERIFY(QMetaObject::invokeMethod(&call, "refreshMembership",
                                          Qt::DirectConnection));
        client.answerPublish(client.lastPublishOp, true,
                             QStringLiteral("delay-9"));
        QVERIFY(call.delayedRefusalReason().isEmpty());
    }

    void aHomeserverRefusingOurMembershipIsNotTheCallServiceRefusingTheCall()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        const quint64 publish = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        QVERIFY(publish != 0);
        client.refusePublish(publish, QStringLiteral("forbidden"));
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Failed));
        const QString roomRefusal = call.lastError();
        QVERIFY(!roomRefusal.isEmpty());

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
        QVERIFY2(roomRefusal.contains(QStringLiteral("permission"))
                     && roomRefusal.contains(QStringLiteral("room")),
                 qPrintable(QStringLiteral(
                                "the membership refusal must point at the "
                                "room's permissions; it said: %1")
                                .arg(roomRefusal)));
        QVERIFY2(!serviceRefusal.contains(QStringLiteral("permission")),
                 qPrintable(QStringLiteral(
                                "the call service's refusal must not read as "
                                "a room permission problem; it said: %1")
                                .arg(serviceRefusal)));
    }

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
        client.emitSfuState(QStringLiteral("signalling"), QString());
        QCOMPARE(failures.size(), 2);
    }

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

    void aParticipantVolumeSurvivesARestart()
    {
        const QString room = QStringLiteral("!vol:example.org");
        const QString identity = QStringLiteral("@her:example.org:HERDEV");
        const QString userId = QStringLiteral("@her:example.org");

        SettingsManager settings;
        settings.saveSession(QStringLiteral("https://example.org"),
                             QStringLiteral("@me:example.org"),
                             QStringLiteral("MEDEV"),
                             QStringLiteral("token-fixture"));

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

        QCOMPARE(call.participantVolume(identity), 200);
    }

    void aVideoInviteIsAskedOfTheEngineWithItsCamera()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:x");
        client.mediaCapable = true;
        FakeMediaBackend media;
        media.videoEngine = true;
        media.streamId = QStringLiteral("stream-1");
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        QVERIFY(controller.placeCall(QStringLiteral("!r:x"),
                                     QStringLiteral("@peer:x"), true));
        QVERIFY(controller.videoCall());
        QCOMPARE(media.prepared.size(), 1);
        QVERIFY(media.prepared.first().endsWith(
            QLatin1String("video=1 camera=1")));
        QVERIFY(controller.cameraOn());
        media.deliverOffer(controller.activeCallId(), QStringLiteral("v=0 o"));
        QVERIFY(client.lastStreamMetadata.contains(
            QLatin1String("\"stream-1\"")));
        QVERIFY(client.lastStreamMetadata.contains(
            QLatin1String("\"video_muted\":false")));
    }

    void anEngineWithoutVideoRefusesAVideoCallAndAnswersAVideoRingAsVoice()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:x");
        client.mediaCapable = true;
        FakeMediaBackend media;
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        QVERIFY(!controller.placeCall(QStringLiteral("!r:x"),
                                      QStringLiteral("@peer:x"), true));
        QCOMPARE(controller.lastRefusal(), QStringLiteral("no_video_engine"));

        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        invite.hasVideo = true;
        controller.setBacklogSuppressed(false);
        client.emitSignal(invite);
        QCOMPARE(controller.state(), CallController::State::Ringing);
        QVERIFY(!controller.videoCall());
    }

    void onlyThePeerMayRenegotiateAndTheAnswerGoesBackAsANegotiate()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:x");
        client.mediaCapable = true;
        FakeMediaBackend media;
        media.videoEngine = true;
        media.streamId = QStringLiteral("stream-1");
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        controller.setBacklogSuppressed(false);
        client.storedDescriptions.insert(invite.eventId, QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QVERIFY(controller.answer());
        media.deliverAnswer(QStringLiteral("call-1"), QStringLiteral("v=0 a"));
        media.deliverConnected(QStringLiteral("call-1"));
        QCOMPARE(controller.state(), CallController::State::Active);

        CallSignal negotiate;
        negotiate.kind = CallSignal::Kind::Negotiate;
        negotiate.roomId = invite.roomId;
        negotiate.callId = invite.callId;
        negotiate.partyId = invite.partyId;
        negotiate.eventId = QStringLiteral("$negotiate");
        negotiate.sender = QStringLiteral("@stranger:x");
        negotiate.sessionType = QStringLiteral("offer");
        negotiate.hasVideo = true;
        client.storedDescriptions.insert(negotiate.eventId, QStringLiteral("v=0 upgrade"));
        client.emitSignal(negotiate);
        QVERIFY(media.negotiations.isEmpty());
        QVERIFY(!controller.videoCall());

        negotiate.sender = invite.sender;
        client.emitSignal(negotiate);
        QCOMPARE(media.negotiations,
                 QStringList{QStringLiteral("offer v=0 upgrade")});
        QVERIFY(controller.videoCall());
        QVERIFY(!controller.cameraOn());

        media.deliverNegotiation(QStringLiteral("call-1"),
                                 QStringLiteral("answer"));
        QCOMPARE(client.sent.last().kind, QStringLiteral("negotiate"));
        QCOMPARE(client.sent.last().extra, QStringLiteral("answer"));
    }

    void anAnswerThatStatesNoChoiceLeavesTheCameraOff()
    {
        RecordingCallClient client;
        client.mediaCapable = true;
        FakeMediaBackend media;
        media.videoEngine = true;
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        invite.hasVideo = true;
        controller.setBacklogSuppressed(false);
        client.storedDescriptions.insert(invite.eventId,
                                         QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QVERIFY(controller.answer());
        QVERIFY(media.prepared.last().endsWith(
            QLatin1String("video=1 camera=0")));
        QVERIFY(!controller.cameraOn());
    }

    void aThirdPersonsInviteIsNotGlare()
    {
        RecordingCallClient client;
        client.mediaCapable = true;
        FakeMediaBackend media;
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        QVERIFY(controller.placeCall(QStringLiteral("!r:x"),
                                     QStringLiteral("@peer:x")));
        const QString ours = controller.activeCallId();
        CallSignal theirs = freshInvite(QString());
        theirs.callId = QStringLiteral("0000-smaller");
        theirs.sender = QStringLiteral("@third:x");
        client.emitSignal(theirs);
        QCOMPARE(controller.activeCallId(), ours);
        QVERIFY(media.answerRequests.isEmpty());
    }

    void turningTheCameraOffBeforeMediaExistsSticks()
    {
        RecordingCallClient client;
        client.deferTurn = true;
        FakeMediaBackend media;
        media.videoEngine = true;
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        controller.setTurnWaitTimeoutForTest(50);
        QVERIFY(controller.placeCall(QStringLiteral("!r:x"),
                                     QStringLiteral("@peer:x"), true));
        QVERIFY(controller.cameraOn());
        QVERIFY(controller.setCameraOn(false));
        QTRY_COMPARE(media.prepared.size(), 1);
        QVERIFY(media.prepared.first().endsWith(
            QLatin1String("video=1 camera=0")));
        QVERIFY(!controller.cameraOn());
    }

    void aPeerThatDeclinesVideoTurnsOurCameraOff()
    {
        RecordingCallClient client;
        client.mediaCapable = true;
        FakeMediaBackend media;
        media.videoEngine = true;
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        QVERIFY(controller.placeCall(QStringLiteral("!r:x"),
                                     QStringLiteral("@peer:x"), true));
        media.deliverOffer(controller.activeCallId(), QStringLiteral("v=0 o"));
        CallSignal answer;
        answer.kind = CallSignal::Kind::Answer;
        answer.roomId = QStringLiteral("!r:x");
        answer.callId = controller.activeCallId();
        answer.partyId = QStringLiteral("peer-party");
        answer.eventId = QStringLiteral("$answer");
        answer.sender = QStringLiteral("@peer:x");
        answer.hasDescription = true;
        client.emitSignal(answer);
        QVERIFY(!controller.cameraOn());
        QVERIFY(!controller.videoCall());
        QCOMPARE(media.cameraRequests, QList<bool>{false});
    }

    void aPeerMuteHidesItsPictureAndAMuteOfOursIsPublished()
    {
        RecordingCallClient client;
        client.simulatedUserId = QStringLiteral("@me:x");
        client.mediaCapable = true;
        FakeMediaBackend media;
        media.videoEngine = true;
        media.streamId = QStringLiteral("stream-1");
        CallController controller;
        controller.setClient(&client);
        controller.setMediaBackend(&media);
        CallSignal invite = freshInvite(QStringLiteral("call-1"));
        invite.hasVideo = true;
        controller.setBacklogSuppressed(false);
        client.storedDescriptions.insert(invite.eventId, QStringLiteral("v=0 offer"));
        client.emitSignal(invite);
        QVERIFY(controller.answer(false));
        QVERIFY(media.prepared.last().endsWith(
            QLatin1String("video=1 camera=0")));
        QVERIFY(!controller.cameraOn());
        media.deliverAnswer(QStringLiteral("call-1"), QStringLiteral("v=0 a"));
        media.deliverConnected(QStringLiteral("call-1"));
        media.deliverRemoteVideo(QStringLiteral("call-1"), true);
        QVERIFY(controller.remoteVideoVisible());

        CallSignal muted;
        muted.kind = CallSignal::Kind::StreamMetadata;
        muted.roomId = invite.roomId;
        muted.callId = invite.callId;
        muted.partyId = invite.partyId;
        muted.eventId = QStringLiteral("$muted");
        muted.sender = invite.sender;
        muted.hasStreamMetadata = true;
        muted.remoteVideoMuted = true;
        muted.remoteAudioMuted = true;
        client.emitSignal(muted);
        QVERIFY(!controller.remoteVideoVisible());
        QVERIFY(controller.remoteMicrophoneMuted());

        const int before = client.sent.size();
        controller.setMicrophoneMuted(true);
        QCOMPARE(client.sent.size(), before + 1);
        QCOMPARE(client.sent.last().kind, QStringLiteral("stream_metadata"));
        QVERIFY(client.lastStreamMetadata.contains(
            QLatin1String("\"audio_muted\":true")));

        QVERIFY(controller.setCameraOn(true));
        QCOMPARE(media.cameraRequests, QList<bool>{true});
        media.deliverCameraFailure(QStringLiteral("call-1"));
        QVERIFY(!controller.cameraOn());
        QVERIFY(client.lastStreamMetadata.contains(
            QLatin1String("\"video_muted\":true")));
        QCOMPARE(controller.state(), CallController::State::Active);
        controller.setMicrophoneMuted(false);
    }

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
        QCOMPARE(call.engineShareVolumeForTest(QStringLiteral("TR_share_a")),
                 -1);

        settings.setCallShareVolume(userId, 60);
        QVERIFY2(call.engineShareVolumeForTest(QStringLiteral("TR_share_a"))
                     != -1,
                 "callShareVolumeChanged has no consumer, so a share level "
                 "changed elsewhere never reaches a live call");
        QCOMPARE(call.engineShareVolumeForTest(QStringLiteral("TR_share_a")),
                 60);
    }

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

        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 1);
        call.setShareVolume(QStringLiteral("TR_share_a"), 35);
        QCOMPARE(call.engineShareVolumeForTest(QStringLiteral("TR_share_a")),
                 -1);

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

        settings.setCallShareVolume(userId, 100);
        QCOMPARE(settings.callShareVolume(userId), 100);

        call.ingestParticipantsForTest({
            sfuParticipant(identity, QStringLiteral("PA_ONE"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        QCOMPARE(call.shareModel()->rowCount(), 1);
        call.setShareVolume(QStringLiteral("TR_share_a"), 35);
        QCOMPARE(call.shareVolume(QStringLiteral("TR_share_a")), 35);
        QCOMPARE(settings.callShareVolume(userId), 35);

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

    void aParticipantVolumeReadsUnityWithNowhereToStoreIt()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        QCOMPARE(call.participantVolume(QStringLiteral("alice")), 100);
    }

    void aSandboxedBuildWithNoDeviceNodeTakesTheCameraPortal()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        for (bool portalUsable : { true, false }) {
            QCOMPARE(SfuCallController::linuxCameraRoute(
true, portalUsable,
false),
                     Route::Portal);
        }
    }

    void aSandboxWithAGrantedDeviceNodeAndNoPortalCameraUsesTheDirectCamera()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        QCOMPARE(SfuCallController::linuxCameraRoute(
true,false,
true),
                 Route::Direct);
    }

    void aSandboxWithAUsablePortalCameraKeepsThePortalWhateverNodesExist()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        QCOMPARE(SfuCallController::linuxCameraRoute(
true,true,
true),
                 Route::Portal);
    }

    void aUsablePortalCameraWinsOverAVisibleDeviceNode()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        QCOMPARE(SfuCallController::linuxCameraRoute(
false,true,
true),
                 Route::Portal);
        QCOMPARE(SfuCallController::linuxCameraRoute(
false,true,
true,true),
                 Route::Direct);
    }

    void aVisibleDeviceNodeKeepsTheDirectCameraWithoutAUsablePortal()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        QCOMPARE(SfuCallController::linuxCameraRoute(
false,false,
true),
                 Route::Direct);
    }

    void withNoDeviceNodeThePortalIsUsedOnlyWhenItIsUsable()
    {
        using Route = SfuCallController::LinuxCameraRoute;
        QCOMPARE(SfuCallController::linuxCameraRoute(
false,true,
false),
                 Route::Portal);
        QCOMPARE(SfuCallController::linuxCameraRoute(
false,false,
false),
                 Route::Direct);
    }

    void aSilentPortalCameraFallsBackOnceToTheGrantedDeviceNode()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        call.setCameraCidForTest(QStringLiteral("cam-portal"));
        call.setCameraRouteForTest(true,
true,
true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("cam-portal")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QVERIFY2(failed.isEmpty(),
                 "a portal camera with a granted device node was given up "
                 "instead of retried on the device");
        QVERIFY(call.cameraOn());
        QVERIFY(call.cameraFellBackForTest());
        QVERIFY(!call.cameraViaPortalForTest());

        call.setCameraCidForTest(QStringLiteral("cam-device"));
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("cam-device")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.at(0).at(0).toString().contains(
                     QStringLiteral("camera sent no picture")),
                 qPrintable(failed.at(0).at(0).toString()));
        QVERIFY(!call.cameraOn());
    }

    void portalFallbackWaitsForRemoteCloseAndCancelsOnHangup()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QList<int> opened;
        call.setCameraPathForTest([&opened](int fd) { opened.append(fd); },
                                  [] {}, true, true);
        call.holdCameraPortalReleaseForTest(true);
        call.setCameraCidForTest(QStringLiteral("portal-1"));
        call.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("portal-1")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QVERIFY(opened.isEmpty());
        call.releaseCameraPortalForTest(QStringLiteral("wrong-cid"));
        QVERIFY(opened.isEmpty());
        call.releaseCameraPortalForTest(QStringLiteral("portal-1"));
        QCOMPARE(opened, QList<int>{-1});

        call.setCameraCidForTest(QStringLiteral("portal-2"));
        call.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("portal-2")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        call.leave();
        call.releaseCameraPortalForTest(QStringLiteral("portal-2"));
        QCOMPARE(opened.size(), 1);
    }

    void portalFallbackHonoursAReleaseReportedDuringTheUnpublish()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QList<int> opened;
        call.setCameraPathForTest([&opened](int fd) { opened.append(fd); },
                                  [] {}, true, true);
        call.holdCameraPortalReleaseForTest(true);
        call.releaseCameraPortalOnUnpublishForTest(true);
        call.setCameraCidForTest(QStringLiteral("portal-sync"));
        call.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("portal-sync")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QVERIFY(call.cameraOn());
        QVERIFY(call.cameraFellBackForTest());
        QCOMPARE(opened, QList<int>{-1});
        call.leave();
    }

    void portalFallbackOpensTheDeviceAfterTheWindowWithoutARelease()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setCameraFallbackWindowForTest(200);
        QList<int> opened;
        call.setCameraPathForTest([&opened](int fd) { opened.append(fd); },
                                  [] {}, true, true);
        call.holdCameraPortalReleaseForTest(true);
        call.setCameraCidForTest(QStringLiteral("portal-silent"));
        call.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("portal-silent")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QVERIFY(opened.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(opened, QList<int>{-1}, 3000);
        QVERIFY(call.cameraOn());
        call.releaseCameraPortalForTest(QStringLiteral("portal-silent"));
        QCOMPARE(opened.size(), 1);

        call.leave();

        SfuCallController second;
        second.setClient(&client);
        second.setMembershipForTest(QStringLiteral("!room:x"), QString());
        second.setCallStateForTest(SfuCallController::State::Connected);
        second.setCameraFallbackWindowForTest(200);
        QList<int> secondOpened;
        second.setCameraPathForTest(
            [&secondOpened](int fd) { secondOpened.append(fd); }, [] {}, true,
            true);
        second.holdCameraPortalReleaseForTest(true);
        second.setCameraCidForTest(QStringLiteral("portal-silent-2"));
        second.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &second, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("portal-silent-2")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QVERIFY2(second.cameraFellBackForTest(),
                 "premise: the second call did not start a device fallback");
        second.leave();
        QTest::qWait(500);
        QVERIFY2(secondOpened.isEmpty(),
                 "the bounded device open ran after the hang-up");
    }

    void portalFallbackRetriesBusyThenPublishesAgain()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QList<int> opened;
        call.setCameraPathForTest([&opened](int fd) { opened.append(fd); },
                                  [] {}, true, true);
        call.setCameraCidForTest(QStringLiteral("portal"));
        call.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("portal")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QCOMPARE(opened.size(), 1);
        const QString busyCid = call.cameraCidForTest();
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, busyCid),
            Q_ARG(QString, QStringLiteral("camera_busy"))));
        QCOMPARE(opened.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 2, 1000);
        QVERIFY(call.cameraOn());
        call.leave();
    }

    void portalFallbackPersistentBusyReportsOnce()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setCameraFallbackWindowForTest(300);
        call.setCameraPathForTest([](int) {}, [] {}, true, true);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        call.setCameraCidForTest(QStringLiteral("portal"));
        call.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("portal")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        for (int attempt = 1; attempt <= 5 && failed.isEmpty(); ++attempt) {
            QTRY_COMPARE_WITH_TIMEOUT(call.cameraPublishesForTest(), attempt,
                                      1000);
            const QString cid = call.cameraCidForTest();
            QVERIFY(QMetaObject::invokeMethod(
                &call, "onEnginePublishFailed", Qt::DirectConnection,
                Q_ARG(QString, cid),
                Q_ARG(QString, QStringLiteral("camera_busy"))));
        }
        QCOMPARE(failed.size(), 1);
        QVERIFY(!call.cameraOn());
        call.leave();
    }

    void aSilentPortalCameraWithoutDeviceAccessNamesThePortal()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        call.setCameraCidForTest(QStringLiteral("cam-1"));
        call.setCameraRouteForTest(true,
true,
false);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("cam-1")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 SfuCallController::cameraPortalNotice(
                     QStringLiteral("camera_portal_no_picture"),
false));
        QVERIFY(!call.cameraOn());
        QVERIFY(!call.cameraFellBackForTest());

        call.setCameraCidForTest(QStringLiteral("cam-2"));
        call.setCameraRouteForTest(true,
false,
false);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("cam-2")),
            Q_ARG(QString, QStringLiteral("camera_failed"))));
        QCOMPARE(failed.count(), 2);
        QCOMPARE(failed.at(1).at(0).toString(),
                 SfuCallController::cameraPortalNotice(
                     QStringLiteral("camera_portal_no_camera"),
false));
    }

    void theCameraFallbackNeedsAPortalThatGaveNothingAndAGrantedNode()
    {
        using Step = SfuCallController::CameraFailureStep;
        for (const char *category : { "camera_no_frames", "camera_failed",
                                      "no_portal", "no_pipewire_remote" }) {
            const QString c = QString::fromLatin1(category);
            QCOMPARE(SfuCallController::cameraFailureStep(c, true, true, false),
                     Step::OpenDeviceNode);
            QCOMPARE(SfuCallController::cameraFailureStep(c, true, false,
                                                          false),
                     Step::Notice);
            QCOMPARE(SfuCallController::cameraFailureStep(c, true, true, true),
                     Step::Notice);
            QCOMPARE(SfuCallController::cameraFailureStep(c, false, true,
                                                          false),
                     Step::Notice);
        }
        for (const char *category : { "not_allowed", "portal_failed",
                                      "timeout", "busy", "camera_unavailable",
                                      "camera_list_unavailable",
                                      "camera_source_closed" }) {
            QCOMPARE(SfuCallController::cameraFailureStep(
                         QString::fromLatin1(category), true, true, false),
                     Step::Notice);
        }
    }

    void aDesktopWithAUsablePortalAsksThePortalForTheCamera()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        CameraPortalCall c;
        c.call.setCameraPathForTest(
            [&c](int fd) { c.published.append(fd); },
            [&c] { ++c.requests; },false,
true);
        c.call.setCameraRouteForTest(false, false,true);
        c.call.setCameraChosenForTest(false);
        c.call.setCameraOn(true);
        QCOMPARE(c.requests, 1);
        QVERIFY(c.call.cameraAwaitingPortalForTest());
        QVERIFY2(c.published.isEmpty(),
                 "the camera was opened on the device node while a usable "
                 "portal camera existed");
        c.call.setCameraOn(false);

        c.call.setCameraChosenForTest(true);
        c.call.setCameraOn(true);
        QCOMPARE(c.requests, 1);
        QCOMPARE(c.published, QList<int>{ -1 });
    }

    void aBusyDirectCameraRetriesOnceThroughThePortal()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        CameraPortalCall c;
        c.call.setCameraPathForTest(
            [&c](int fd) { c.published.append(fd); },
            [&c] { ++c.requests; },false,
false);
        c.call.setCameraRouteForTest(false, false,true);
        c.call.setCameraChosenForTest(false);
        QSignalSpy failed(&c.call, &SfuCallController::callFailed);
        c.call.setCameraOn(true);
        QCOMPARE(c.published, QList<int>{ -1 });
        QCOMPARE(c.requests, 0);

        QVERIFY(QMetaObject::invokeMethod(
            &c.call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, c.call.cameraCidForTest()),
            Q_ARG(QString, QStringLiteral("camera_busy"))));
        QVERIFY2(failed.isEmpty(), "told the user before trying the portal");
        QCOMPARE(c.requests, 1);
        QVERIFY(c.call.cameraFellBackToPortalForTest());
        QVERIFY(c.call.cameraAwaitingPortalForTest());
        QVERIFY(c.call.cameraOn());

        const int fd = CameraPortalCall::portalFd();
        QVERIFY(fd >= 0);
        Q_EMIT c.portal.ready(fd);
        QCOMPARE(c.published, (QList<int>{ -1, fd }));
        QVERIFY(c.call.cameraViaPortalForTest());

        c.call.leave();
        QVERIFY(!c.call.cameraFellBackToPortalForTest());

        CameraPortalCall chosen;
        chosen.call.setCameraPathForTest(
            [&chosen](int fd) { chosen.published.append(fd); },
            [&chosen] { ++chosen.requests; },false,
false);
        chosen.call.setCameraRouteForTest(false, false, true);
        chosen.call.setCameraChosenForTest(true);
        QSignalSpy chosenFailed(&chosen.call, &SfuCallController::callFailed);
        chosen.call.setCameraOn(true);
        QVERIFY(QMetaObject::invokeMethod(
            &chosen.call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, chosen.call.cameraCidForTest()),
            Q_ARG(QString, QStringLiteral("camera_busy"))));
        QCOMPARE(chosen.requests, 0);
        QCOMPARE(chosenFailed.count(), 1);
        QVERIFY(!chosen.call.cameraOn());
    }

    void aChosenCameraIsNeverSubstitutedEvenWhenItsNodeIsMissing()
    {
        for (const bool sandboxed : {false, true}) {
            QCOMPARE(SfuCallController::linuxCameraRoute(
                         sandboxed, true, false, true),
                     SfuCallController::LinuxCameraRoute::Direct);
        }
    }

    void aPortalCaptureErrorStillRetriesTheDeviceNode()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        CameraPortalCall c;
        c.call.setCameraRouteForTest(true, true, true);
        c.call.setCameraChosenForTest(false);
        c.call.setCameraOn(true);
        const int fd = CameraPortalCall::portalFd();
        QVERIFY(fd >= 0);
        Q_EMIT c.portal.ready(fd);
        QSignalSpy failed(&c.call, &SfuCallController::callFailed);
        QVERIFY(QMetaObject::invokeMethod(
            &c.call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, c.call.cameraCidForTest()),
            Q_ARG(QString, QStringLiteral("camera_failed"))));
        QCOMPARE(c.requests, 1);
        QCOMPARE(c.published.last(), -1);
        QVERIFY(c.call.cameraFellBackForTest());
        QVERIFY(!c.call.cameraViaPortalForTest());
        QVERIFY(c.call.cameraOn());
        QVERIFY(failed.isEmpty());
    }

    void aRememberedPortalFallbackCannotReplaceANewCameraChoice()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        for (const bool preferredMissing : {false, true}) {
            CameraPortalCall c;
            c.call.setCameraPathForTest(
                [&c](int fd) { c.published.append(fd); },
                [&c] { ++c.requests; }, false, false);
            c.call.setCameraRouteForTest(false, false, true);
            c.call.setCameraChosenForTest(false);
            c.call.setCameraOn(true);
            QVERIFY(QMetaObject::invokeMethod(
                &c.call, "onEnginePublishFailed", Qt::DirectConnection,
                Q_ARG(QString, c.call.cameraCidForTest()),
                Q_ARG(QString, QStringLiteral("camera_busy"))));
            const int fd = CameraPortalCall::portalFd();
            QVERIFY(fd >= 0);
            Q_EMIT c.portal.ready(fd);
#ifdef Q_OS_UNIX
            ::close(fd);
#endif
            QVERIFY(c.call.cameraFellBackToPortalForTest());
            QCOMPARE(c.requests, 1);
            c.call.setCameraChosenForTest(true);
            c.call.setCallStateForTest(SfuCallController::State::Reconnecting);
            c.call.setCallStateForTest(SfuCallController::State::Connected);
            c.call.restartCameraCaptureForTest(!preferredMissing);
            QCOMPARE(c.requests, 1);
            QCOMPARE(c.published.last(), -1);
            QVERIFY(!c.call.cameraViaPortalForTest());
            if (preferredMissing) {
                QSignalSpy failed(&c.call, &SfuCallController::callFailed);
                QVERIFY(QMetaObject::invokeMethod(
                    &c.call, "onEnginePublishFailed", Qt::DirectConnection,
                    Q_ARG(QString, c.call.cameraCidForTest()),
                    Q_ARG(QString, QStringLiteral("camera_unavailable"))));
                QCOMPARE(failed.count(), 1);
                QVERIFY(!c.call.cameraOn());
                QCOMPARE(c.requests, 1);
            }
        }
    }

    void aBusyCameraTheUserChoseOrWithNoPortalIsNamedNotRetried()
    {
        using C = SfuCallController;
        QVERIFY(C::directCameraFallsBackToPortal(
            QStringLiteral("camera_busy"), false, true, false));
        QVERIFY(C::directCameraFallsBackToPortal(
            QStringLiteral("camera_resource"), false, true, false));
        QVERIFY(!C::directCameraFallsBackToPortal(
            QStringLiteral("camera_busy"), true, true, false));
        QVERIFY(!C::directCameraFallsBackToPortal(
            QStringLiteral("camera_busy"), false, false, false));
        QVERIFY(!C::directCameraFallsBackToPortal(
            QStringLiteral("camera_busy"), false, true, true));
        QVERIFY(!C::directCameraFallsBackToPortal(
            QStringLiteral("camera_failed"), false, true, false));

        for (const bool chosen : { false, true }) {
            RecordingCallClient client;
            SfuCallController call;
            call.setClient(&client);
            call.setMembershipForTest(QStringLiteral("!room:x"), QString());
            call.setCallStateForTest(SfuCallController::State::Connected);
            call.setCameraChosenForTest(chosen);
            QSignalSpy failed(&call, &SfuCallController::callFailed);
            call.setCameraCidForTest(QStringLiteral("cam-d"));
            call.setCameraRouteForTest(false, false, true);
            QVERIFY(QMetaObject::invokeMethod(
                &call, "onEnginePublishFailed", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("cam-d")),
                Q_ARG(QString, QStringLiteral("camera_busy"))));
            QCOMPARE(failed.count(), 1);
            QVERIFY2(failed.at(0).at(0).toString().contains(
                         QStringLiteral("may be in use by another app")),
                     qPrintable(failed.at(0).at(0).toString()));
            QVERIFY2(failed.at(0).at(0).toString().contains(
                         QStringLiteral("unplugged")),
                     qPrintable(failed.at(0).at(0).toString()));
            QVERIFY(!call.cameraOn());
        }
    }

    void theCameraPortalNoticesNameWhatHappenedAndTheGrant()
    {
        using C = SfuCallController;
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("camera_no_frames"),
                                         true, true),
                 QStringLiteral("camera_portal_no_picture"));
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("camera_no_frames"),
                                         true, false),
                 QStringLiteral("camera_portal_no_camera"));
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("camera_no_frames"),
                                         false, true),
                 QStringLiteral("camera_no_frames"));
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("no_portal"), true,
                                         false),
                 QStringLiteral("camera_portal_unavailable"));
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("timeout"), true,
                                         true),
                 QStringLiteral("camera_failed"));
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("not_allowed"), true,
                                         true),
                 QStringLiteral("camera_not_allowed"));
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("portal_failed"), true,
                                         true),
                 QStringLiteral("camera_failed"));
        QCOMPARE(C::cameraNoticeCategory(QStringLiteral("camera_unavailable"),
                                         true, true),
                 QStringLiteral("camera_unavailable"));
        for (const char *category :
             { "camera_portal_no_camera", "camera_portal_no_picture",
               "camera_portal_unavailable" }) {
            const QString c = QString::fromLatin1(category);
            const QString flatpak = C::cameraPortalNotice(c, true);
            const QString other = C::cameraPortalNotice(c, false);
            QVERIFY2(flatpak.contains(QStringLiteral("Flatseal"))
                         && flatpak.contains(QStringLiteral("All devices")),
                     qPrintable(flatpak));
            QVERIFY2(!other.isEmpty()
                         && !other.contains(QStringLiteral("Flatseal")),
                     qPrintable(other));
        }
        QVERIFY(C::cameraPortalNotice(QStringLiteral("camera_failed"), true)
                    .isEmpty());
    }

    void aCameraTheDesktopRefusedIsNeverOpenedThroughTheDevice()
    {
        RecordingCallClient client;
        SfuCallController call;
        CameraPortal portal;
        call.setClient(&client);
        call.setCameraPortal(&portal);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        call.setCameraCidForTest(QString());
        call.setCameraRouteForTest(true,
true,
true);
        call.setCameraAwaitingPortalForTest();
        Q_EMIT portal.failed(QStringLiteral("not_allowed"));
        QVERIFY(!call.cameraFellBackForTest());
        QVERIFY(!call.cameraOn());
        QCOMPARE(failed.count(), 1);
        QVERIFY2(failed.at(0).at(0).toString().contains(
                     QStringLiteral("turned off in your system settings")),
                 qPrintable(failed.at(0).at(0).toString()));

        call.setCameraRouteForTest(true, true, true);
        call.setCameraAwaitingPortalForTest();
        Q_EMIT portal.failed(QStringLiteral("no_portal"));
        QVERIFY(call.cameraFellBackForTest());
        QVERIFY(call.cameraOn());
        QCOMPARE(failed.count(), 1);
    }

    void aPortalCameraPublishesOnTheGrantWithTheDescriptorItWasGiven()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        CameraPortalCall c;
        c.call.setCameraOn(true);
        QCOMPARE(c.requests, 1);
        QVERIFY(c.call.cameraAwaitingPortalForTest());
        QVERIFY2(c.call.cameraOn(), "the button does not answer the press");
        QVERIFY2(c.published.isEmpty(),
                 "a camera was declared before the portal granted it");

        const int fd = CameraPortalCall::portalFd();
        QVERIFY(fd >= 0);
        Q_EMIT c.portal.ready(fd);
        QCOMPARE(c.published, QList<int>{ fd });
        QVERIFY2(c.publishedOpen.value(0),
                 "the controller closed the descriptor it handed over");
        QVERIFY(!c.call.cameraAwaitingPortalForTest());
        QVERIFY(c.call.cameraOn());
        QVERIFY(c.call.cameraViaPortalForTest());
        QCOMPARE(c.call.cameraPublishesForTest(), 1);
    }

    void aGrantForACameraTurnedOffDuringTheDialogIsClosedNotPublished()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        CameraPortalCall c;
        c.call.setCameraOn(true);
        QCOMPARE(c.requests, 1);
        c.call.setCameraOn(false);
        QVERIFY(!c.call.cameraAwaitingPortalForTest());
        QVERIFY(!c.call.cameraOn());

        const int late = CameraPortalCall::portalFd();
        QVERIFY(late >= 0);
        Q_EMIT c.portal.ready(late);
        QVERIFY2(c.published.isEmpty(),
                 "a camera the user switched off was published anyway");
        QVERIFY2(!CameraPortalCall::fdOpen(late),
                 "the late grant's PipeWire descriptor leaked");

        c.call.setCameraOn(true);
        QCOMPARE(c.requests, 2);
        const int fd = CameraPortalCall::portalFd();
        Q_EMIT c.portal.ready(fd);
        QCOMPARE(c.published, QList<int>{ fd });
        QVERIFY(c.call.cameraOn());
    }

    void leavingDuringTheCameraDialogAbandonsItAndClosesALateGrant()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        CameraPortalCall c;
        c.call.setCameraOn(true);
        QVERIFY(c.call.cameraAwaitingPortalForTest());
        c.call.leave();
        QVERIFY2(!c.call.cameraAwaitingPortalForTest(),
                 "the call ended with the camera still waiting on the portal");
        QVERIFY(!c.call.cameraOn());
        QVERIFY(!c.portal.busy());

        const int late = CameraPortalCall::portalFd();
        Q_EMIT c.portal.ready(late);
        QVERIFY(c.published.isEmpty());
        QVERIFY2(!CameraPortalCall::fdOpen(late),
                 "a grant for a call that ended leaked its descriptor");
    }

    void aGrantAfterTheCallStoppedBeingActiveIsClosedAndUnwound()
    {
#ifndef Q_OS_UNIX
        QSKIP("portal descriptors exist on Unix only");
#endif
        CameraPortalCall c;
        c.call.setCameraOn(true);
        QVERIFY(c.call.cameraAwaitingPortalForTest());
        c.call.setCallStateForTest(SfuCallController::State::Idle);
        QVERIFY(!c.call.active());

        const int fd = CameraPortalCall::portalFd();
        Q_EMIT c.portal.ready(fd);
        QVERIFY(c.published.isEmpty());
        QVERIFY2(!CameraPortalCall::fdOpen(fd),
                 "the grant's descriptor leaked on the inactive-call exit");
        QVERIFY(!c.call.cameraAwaitingPortalForTest());
        QVERIFY2(!c.call.cameraOn(),
                 "the camera stayed lit with nothing published");
    }

    void aDeclinedCameraDialogUnwindsTheControlSilently()
    {
        CameraPortalCall c;
        QSignalSpy failed(&c.call, &SfuCallController::callFailed);
        c.call.setCameraOn(true);
        QCOMPARE(c.requests, 1);
        Q_EMIT c.portal.cancelled();
        QVERIFY(!c.call.cameraAwaitingPortalForTest());
        QVERIFY(!c.call.cameraOn());
        QVERIFY(c.published.isEmpty());
        QCOMPARE(failed.count(), 0);
    }

    void theCameraFallbackLatchDoesNotOutliveTheCall()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!room:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setCameraCidForTest(QStringLiteral("cam-portal"));
        call.setCameraRouteForTest(true, true, true);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEnginePublishFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("cam-portal")),
            Q_ARG(QString, QStringLiteral("camera_no_frames"))));
        QVERIFY(call.cameraFellBackForTest());
        call.leave();
        QVERIFY2(!call.cameraFellBackForTest(),
                 "the portal fallback outlived the call that needed it");
    }

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
true, platform, session, wayland, x11,
                element);
        };
        QCOMPARE(route(QStringLiteral("wayland"), QStringLiteral("wayland"),
                       QStringLiteral("wayland-0"), QString(), false),
                 Route::Portal);
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("x11"), QString(),
                       QStringLiteral(":0"), true),
                 Route::Portal);
        QCOMPARE(route(QString(), QString(), QString(), QString(), false),
                 Route::Portal);
        QVERIFY(SfuCallController::linuxShareRefusal(Route::Portal).isEmpty());
    }

    void aWaylandSessionWithNoPortalRefusesInsteadOfOfferingAPickerItCannotHonour()
    {
        using Route = SfuCallController::LinuxShareRoute;
        const auto route = [](const QString &platform, const QString &session,
                              const QString &wayland) {
            return SfuCallController::linuxShareRoute(
false, platform, session, wayland,
QStringLiteral(":0"),
true);
        };
        QCOMPARE(route(QStringLiteral("wayland"), QString(), QString()),
                 Route::RefuseWaylandNeedsPortal);
        QCOMPARE(route(QStringLiteral("wayland-egl"), QString(), QString()),
                 Route::RefuseWaylandNeedsPortal);
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("wayland"),
                       QString()),
                 Route::RefuseWaylandNeedsPortal);
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("Wayland"),
                       QString()),
                 Route::RefuseWaylandNeedsPortal);
        QCOMPARE(route(QStringLiteral("xcb"), QStringLiteral("x11"),
                       QStringLiteral("wayland-0")),
                 Route::RefuseWaylandNeedsPortal);

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

    void aRecognisedX11PortalRefusalFallsBackToLightningsOwnPicker()
    {
        const auto falls = [](const QString &category, const QString &platform,
                              const QString &session, const QString &wayland,
                              const QString &x11, bool element) {
            return SfuCallController::portalFailureFallsBackToDisplays(
                category, platform, session, wayland, x11, element);
        };
        const QString refusal = QStringLiteral("x11_unsupported");
        QVERIFY(falls(refusal, QStringLiteral("xcb"), QStringLiteral("x11"),
                      QString(), QStringLiteral(":0"), true));
        QVERIFY(falls(refusal, QStringLiteral("xcb"), QString(), QString(),
                      QStringLiteral(":0"), true));
        for (const QString &other :
             { QStringLiteral("refused"), QStringLiteral("no_portal"),
               QStringLiteral("start_failed"), QStringLiteral("select_failed"),
               QStringLiteral("no_session"), QStringLiteral("busy"),
               QStringLiteral("no_stream") }) {
            QVERIFY2(!falls(other, QStringLiteral("xcb"),
                            QStringLiteral("x11"), QString(),
                            QStringLiteral(":0"), true),
                     qPrintable(other));
        }
        QVERIFY(!falls(refusal, QStringLiteral("wayland"),
                       QStringLiteral("wayland"), QStringLiteral("wayland-0"),
                       QString(), true));
        QVERIFY(!falls(refusal, QStringLiteral("xcb"),
                       QStringLiteral("wayland"), QString(),
                       QStringLiteral(":0"), true));
        QVERIFY(!falls(refusal, QStringLiteral("xcb"), QStringLiteral("x11"),
                       QStringLiteral("wayland-0"), QStringLiteral(":0"),
                       true));
        QVERIFY(!falls(refusal, QStringLiteral("xcb"), QStringLiteral("x11"),
                       QString(), QStringLiteral(":0"), false));
        QVERIFY(!falls(refusal, QStringLiteral("xcb"), QStringLiteral("x11"),
                       QString(), QString(), true));
        QCOMPARE(SfuCallController::linuxShareRoute(
                     true, QStringLiteral("xcb"), QStringLiteral("x11"),
                     QString(), QStringLiteral(":0"), true),
                 SfuCallController::LinuxShareRoute::Portal);
    }

    void aPortalThatNeverAnswersOnX11FallsBackToLightningsOwnPicker()
    {
        for (const QString &category :
             { QStringLiteral("no_answer"), QStringLiteral("timeout"),
               QStringLiteral("session_refused") }) {
            QVERIFY2(SfuCallController::portalFailureFallsBackToDisplays(
                         category, QStringLiteral("xcb"), QStringLiteral("x11"),
                         QString(), QStringLiteral(":0"), true),
                     qPrintable(category));
            QVERIFY2(!SfuCallController::portalFailureFallsBackToDisplays(
                         category, QStringLiteral("wayland"),
                         QStringLiteral("wayland"), QStringLiteral("wayland-0"),
                         QString(), true),
                     qPrintable(category));
            QVERIFY2(!SfuCallController::portalFailureFallsBackToDisplays(
                         category, QStringLiteral("xcb"), QStringLiteral("x11"),
                         QStringLiteral("wayland-0"), QStringLiteral(":0"),
                         true),
                     qPrintable(category));
        }
        QFile ctl(QStringLiteral(SOURCE_DIR "/src/calls/SfuCallController.cpp"));
        QVERIFY(ctl.open(QIODevice::ReadOnly));
        const QByteArray c = ctl.readAll();
        const int route = c.indexOf("case LinuxShareRoute::Portal: {");
        QVERIFY2(route > 0, "the Portal route lost its bounded block");
        const QByteArray block = c.mid(route, 2400);
        QVERIFY2(block.contains("portalFailureFallsBackToDisplays(")
                     && block.contains("QStringLiteral(\"no_answer\")")
                     && block.contains("x11FallbackPossible ? "
                                       "kX11PortalPreparationBoundMs : 0"),
                 "requestScreenShare() does not bound the portal on X11");
        QFile portal(QStringLiteral(SOURCE_DIR "/src/calls/ScreenCastPortal.cpp"));
        QVERIFY(portal.open(QIODevice::ReadOnly));
        const QByteArray p = portal.readAll();
        QVERIFY2(p.contains("Q_EMIT failed(QStringLiteral(\"no_answer\"))"),
                 "the portal has no no-answer bound");
        QVERIFY2(p.contains("Q_EMIT failed(QStringLiteral(\"session_refused\"))"),
                 "a refused session is still swallowed as a cancel");
        const int start = p.indexOf("void ScreenCastPortal::startSession()");
        QVERIFY(start > 0);
        QVERIFY2(p.mid(start, 600).contains("armAnswerWatchdog(m_pickerBoundMs)"),
                 "the picker stage is not bounded");
    }

    void theFallbackAlwaysShowsThePickerAndOnlyRecognisesTheRefusal()
    {
        QFile ctl(QStringLiteral(SOURCE_DIR "/src/calls/SfuCallController.cpp"));
        QVERIFY(ctl.open(QIODevice::ReadOnly));
        const QByteArray c = ctl.readAll();
        const int at = c.indexOf("portal failed category=");
        QVERIFY(at > 0);
        const QByteArray handler = c.mid(at, 1800);
        QVERIFY2(handler.contains("portalFailureFallsBackToDisplays(")
                     && handler.contains(
                         "offerLinuxDisplayPicker(/*autoSelectSingle=*/false)"),
                 "the portal failure handler does not fall back to an "
                 "always-asking picker");
        QVERIFY2(c.contains("offerLinuxDisplayPicker(/*autoSelectSingle=*/true)"),
                 "the no-portal route lost its single-display shortcut");
        QFile portal(QStringLiteral(SOURCE_DIR "/src/calls/ScreenCastPortal.cpp"));
        QVERIFY(portal.open(QIODevice::ReadOnly));
        const QByteArray p = portal.readAll();
        QVERIFY2(p.contains("x11_unsupported") && p.contains("errorCategory("),
                 "the portal never names the X11 refusal");
    }

    void anX11SessionWithNoPortalFallsBackToLightningsOwnPicker()
    {
        using Route = SfuCallController::LinuxShareRoute;
        QCOMPARE(SfuCallController::linuxShareRoute(
false, QStringLiteral("xcb"),
                     QStringLiteral("x11"),QString(),
                     QStringLiteral(":0"),true),
                 Route::FallbackDisplays);
        QCOMPARE(SfuCallController::linuxShareRoute(
                     false, QStringLiteral("xcb"), QString(), QString(),
                     QStringLiteral(":1"), true),
                 Route::FallbackDisplays);
        QVERIFY(SfuCallController::linuxShareRefusal(Route::FallbackDisplays)
                    .isEmpty());
    }

    void anX11SessionWithoutTheCaptureElementRefusesBeforeOfferingAPicker()
    {
        using Route = SfuCallController::LinuxShareRoute;
        QCOMPARE(SfuCallController::linuxShareRoute(
                     false, QStringLiteral("xcb"), QStringLiteral("x11"),
                     QString(), QStringLiteral(":0"),
false),
                 Route::RefuseNoCaptureElement);
        const QString message = SfuCallController::linuxShareRefusal(
            Route::RefuseNoCaptureElement);
        QVERIFY2(message.contains(QStringLiteral("ximagesrc")),
                 qPrintable(QStringLiteral("the refusal does not name the "
                                           "missing element: %1")
                                .arg(message)));

        const QString sandboxed = SfuCallController::linuxShareRefusal(
            Route::RefuseNoCaptureElement,true);
        QVERIFY2(!sandboxed.contains(QStringLiteral("gst-plugins-good")),
                 qPrintable(QStringLiteral("a sandboxed build is told to "
                                           "install a host package: %1")
                                .arg(sandboxed)));
        QVERIFY2(sandboxed.contains(QStringLiteral("xdg-desktop-portal")),
                 qPrintable(QStringLiteral("the sandboxed refusal does not "
                                           "name the portal: %1")
                                .arg(sandboxed)));
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
        QVERIFY(message.contains(QStringLiteral("gst-plugins-good")));
        QCOMPARE(SfuCallController::linuxShareRefusal(
                     Route::RefuseWaylandNeedsPortal, true),
                 SfuCallController::linuxShareRefusal(
                     Route::RefuseWaylandNeedsPortal, false));

        QCOMPARE(SfuCallController::linuxShareRoute(false, QStringLiteral("xcb"),
                                                    QString(), QString(),
QString(),
                                                    true),
                 Route::RefuseNoDisplayServer);
        QVERIFY(!SfuCallController::linuxShareRefusal(
                     Route::RefuseNoDisplayServer)
                     .isEmpty());
    }

    void theLinuxCaptureRectangleIsNeverDerivedFromDevicePixelRatio()
    {
        QFile file(QStringLiteral(
            SOURCE_DIR "/src/calls/SfuCallController.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "SfuCallController.cpp is not where this test looks for it");
        const QString source = QString::fromUtf8(file.readAll());

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
        QCOMPARE(SfuCallController::validX11CaptureRect(
                     QRect(0, 0, 3840, 2160)),
                 QRect(0, 0, 3840, 2160));
        QCOMPARE(SfuCallController::validX11CaptureRect(
                     QRect(3840, 0, 3840, 2160)),
                 QRect(3840, 0, 3840, 2160));
        QCOMPARE(SfuCallController::validX11CaptureRect(
                     QRect(0, 2160, 1920, 1080)),
                 QRect(0, 2160, 1920, 1080));

        QVERIFY(!SfuCallController::validX11CaptureRect(
                     QRect(-1920, 0, 1920, 1080))
                     .isValid());
        QVERIFY(!SfuCallController::validX11CaptureRect(
                     QRect(0, -100, 800, 600))
                     .isValid());
        QVERIFY(!SfuCallController::validX11CaptureRect(QRect(0, 0, 0, 0))
                     .isValid());
        QVERIFY(!SfuCallController::validX11CaptureRect(QRect()).isValid());

        QVERIFY(!SfuCallController::nativeScreenRect(nullptr).isValid());
    }

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
        hangup.sender = QStringLiteral("@stranger:x");
        hangup.callId = QStringLiteral("call-1");
        hangup.partyId = QStringLiteral("stranger-party");
        hangup.reason = QStringLiteral("user_hangup");
        client.emitSignal(hangup);
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(endedSpy.count(), 0);

        CallSignal selected = hangup;
        selected.kind = CallSignal::Kind::SelectAnswer;
        selected.eventId = QStringLiteral("$select-stranger");
        selected.selectedPartyId = QStringLiteral("somebody-else");
        client.emitSignal(selected);
        QCOMPARE(calls.state(), CallController::State::Ringing);

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

    void everySfuFailureCategoryHasItsOwnHonestWording()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        auto sentenceFor = [&](const QString &category) {
            call.setCallStateForTest(SfuCallController::State::Connecting);
            client.emitSfuState(QStringLiteral("failed"), category);
            return call.lastError();
        };

        const QString generic =
            sentenceFor(QStringLiteral("a_category_no_path_emits"));
        QVERIFY(!generic.isEmpty());

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

        QStringList distinct = saidForGroup.values();
        distinct.sort();
        QStringList deduped = distinct;
        deduped.removeDuplicates();
        QVERIFY2(distinct == deduped,
                 "two failure groups produce the same sentence, so the user "
                 "still cannot tell them apart");

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

        QVERIFY2(saidForGroup.value(QStringLiteral("private"))
                     != saidForGroup.value(QStringLiteral("connect")),
                 "a focus refused for having a private address still reads "
                 "as 'the network is down'");
    }

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
        QVERIFY(said.contains(QStringLiteral("permission")));
        QVERIFY(said.contains(QStringLiteral("room")));
        QVERIFY2(!said.contains(QStringLiteral("in the room's permissions")),
                 qPrintable(QStringLiteral(
                                "the refusal still points at a permissions "
                                "screen that cannot set call membership; it "
                                "said: %1").arg(said)));
    }

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

        call.leave();
        QVERIFY2(client.retractions.isEmpty(),
                 "a retraction was sent for a membership that does not exist "
                 "yet — and when the server refuses it, the give-up branch "
                 "reports a ghost membership nobody ever created");

        client.answerPublish(publish, true, QStringLiteral("delay-1"));

        QCOMPARE(client.retractions.size(), 1);
        QCOMPARE(client.retractions.first().first, room);
        QVERIFY2(client.retractions.first().second
                     == QStringLiteral("delay-1"),
                 "the retraction did not carry the delay id THIS publish "
                 "armed, so the server's delayed retraction is left running "
                 "against an id nothing holds");
    }

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

    void aStalePublishDoesNotRetractTheCallWeAreBackIn()
    {
        const QString room = QStringLiteral("!room:example.org");
        const QString focus = QStringLiteral("https://sfu.example.org");
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);

        const quint64 first = call.beginMembershipPublishForTest(room, focus);
        call.leave();
        const quint64 second = call.beginMembershipPublishForTest(room, focus);
        QVERIFY(second != first);
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Preparing));

        client.answerPublish(first, true, QStringLiteral("delay-1"));

        QVERIFY2(client.retractions.isEmpty(),
                 "the stale publish's answer retracted the state event the "
                 "call we are in right now owns");
        QCOMPARE(static_cast<int>(call.state()),
                 static_cast<int>(SfuCallController::State::Preparing));
    }

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
        QCOMPARE(participantRowFor(model, identity), -1);
        client.emitHandChanged(room, QStringLiteral("@bea:example.org"),
                               membership, QStringLiteral("$raise"), true);

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

        client.emitHandChanged(room, QString(), QString(),
                               QStringLiteral("$raise"), false);
        QCOMPARE(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 false);
    }

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

        client.emitHandChanged(room, QStringLiteral("@mallory:example.org"),
                               membership, QStringLiteral("$forged"), true);
        QCOMPARE(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::HandRaisedRole)
                     .toBool(),
                 false);

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

        const QString customEmoji = QString::fromUtf8("🦊");
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, customEmoji);
        QCOMPARE(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 customEmoji);

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

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kPartyEmoji);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kThumbsUpEmoji);

        QTRY_VERIFY(participantRole(model, row,
                                    CallParticipantModel::ReactionEmojiRole)
                        .toString()
                        .isEmpty());

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kPartyEmoji);
        QCOMPARE(participantRole(model, row,
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 kPartyEmoji);
    }

    void aForgedCallReactionIsNeitherShownNorParked()
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

        client.emitCallReaction(room, QStringLiteral("@mallory:example.org"),
                                membership, kThumbsUpEmoji);
        QVERIFY2(participantRole(model, participantRowFor(model, identity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "a reaction from somebody who does not own the membership it "
                 "references was drawn on that membership's owner");

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

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, QString());
        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                QStringLiteral("$nobodys-membership"),
                                kThumbsUpEmoji);
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

        client.emitCallReaction(room, QStringLiteral("@bea:example.org"),
                                membership, kThumbsUpEmoji);
        QVERIFY(participantRole(model, participantRowFor(model, identity),
                                CallParticipantModel::ReactionEmojiRole)
                    .toString()
                    .isEmpty());

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

        for (const QString &invalid : {
                 QStringLiteral("mxc://example.org/image"),
                 QStringLiteral("text"),
                 QString::fromUtf8("🦊🎉"),
                 QString(40, QChar(0x2600)),
             }) {
            call.sendCallReaction(invalid, QString());
        }
        QCOMPARE(client.reactionSends.size(), 0);

        call.sendCallReaction(kThumbsUpEmoji,
                              QStringLiteral("thumbsup"));
        QCOMPARE(client.reactionSends.size(), 1);
        QCOMPARE(client.reactionSends.at(0).roomId, room);
        QCOMPARE(client.reactionSends.at(0).membershipEventId, ownMembership);
        QCOMPARE(client.reactionSends.at(0).emoji,
                 kThumbsUpEmoji);
        QCOMPARE(client.reactionSends.at(0).name,
                 QStringLiteral("thumbsup"));

        QVERIFY2(participantRole(model, participantRowFor(model, ownIdentity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString()
                     .isEmpty(),
                 "the sender's own tile was lit before the event existed");

        call.sendCallReaction(kPartyEmoji,
                              QStringLiteral("party"));
        QCOMPARE(client.reactionSends.size(), 1);

        client.answerRtcSend(client.lastReactionOp, false,
                             QStringLiteral("network"));
        const QString pickedEmoji = QString::fromUtf8("🦊");
        call.sendCallReaction(pickedEmoji, QString());
        QCOMPARE(client.reactionSends.size(), 2);
        QCOMPARE(client.reactionSends.at(1).emoji, pickedEmoji);
        QCOMPARE(client.reactionSends.at(1).name, QString());

        client.emitCallReaction(room, QStringLiteral("@me:example.org"),
                                ownMembership,
                                pickedEmoji);
        QCOMPARE(participantRole(model, participantRowFor(model, ownIdentity),
                                 CallParticipantModel::ReactionEmojiRole)
                     .toString(),
                 pickedEmoji);
    }

    void theInformationEmojiFromThePickerCanBeSent()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        const quint64 op = call.beginMembershipPublishForTest(
            QStringLiteral("!room:example.org"),
            QStringLiteral("https://sfu.example.org"));
        client.answerPublish(op, true, QString());
        call.setCallStateForTest(SfuCallController::State::Connected);

        const QString emoji = QString::fromUtf8("ℹ️");
        QVERIFY(emoji.at(0).isLetter());
        call.sendCallReaction(emoji, QString());
        QCOMPARE(client.reactionSends.size(), 1);
        QCOMPARE(client.reactionSends.first().emoji, emoji);
        QCOMPARE(client.reactionSends.first().name, QString());
    }

    void aShareAudioFailureCostsTheSoundAndNotTheCall()
    {
        QVERIFY(SfuCallController::categoryIsShareAudioOnly(
            QStringLiteral("share_audio_failed")));
        QVERIFY(SfuCallController::categoryIsShareAudioOnly(
            QStringLiteral("share_audio_unavailable")));
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
        QCOMPARE(failures.count(), 1);
        const QString sentence = failures.at(0).at(0).toString();
        QVERIFY2(!sentence.isEmpty(), "no wording for share_audio_failed");
        QVERIFY2(!sentence.contains(QStringLiteral("call ended"),
                                    Qt::CaseInsensitive),
                 qPrintable(QStringLiteral(
                     "a share-audio failure tells the user the call ended: %1")
                     .arg(sentence)));

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

        SfuCallController fatal;
        fatal.setClient(&client);
        fatal.setCallStateForTest(SfuCallController::State::Connected);
        QVERIFY(QMetaObject::invokeMethod(
            &fatal, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("connection_lost"))));
        QCOMPARE(fatal.state(), SfuCallController::State::Failed);
    }

    void chosenAppsThatCannotBeCapturedCostOnlyTheSound()
    {
        QVERIFY(SfuCallController::categoryIsShareAudioOnly(
            QStringLiteral("share_audio_apps_unavailable")));
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setCallStateForTest(SfuCallController::State::Connected);
        QSignalSpy failures(&call, &SfuCallController::callFailed);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("share_audio_apps_unavailable"))));
        QVERIFY(call.active());
        QCOMPARE(failures.count(), 1);
        QVERIFY2(failures.at(0).at(0).toString().contains(
                     QStringLiteral("apps"), Qt::CaseInsensitive),
                 qPrintable(failures.at(0).at(0).toString()));
    }

    void theShareSoundChoiceIsAnEnumThatNeverWidens()
    {
        SfuCallController call;
        QCOMPARE(call.shareAudioMode(), 1);
        QVERIFY(call.shareAudioEnabled());

        call.setShareAudioMode(7);
        QCOMPARE(call.shareAudioMode(), 0);
        QVERIFY(!call.shareAudioEnabled());
        call.setShareAudioEnabled(true);
        QCOMPARE(call.shareAudioMode(), 1);

        call.setShareAudioCanChooseAppsForTest(false);
        call.setShareAudioMode(2);
        QCOMPARE(call.shareAudioMode(), 1);
        call.setShareAudioAppChosen(QStringLiteral("firefox"),
                                    QStringLiteral("Firefox"), true);
        QCOMPARE(call.shareAudioMode(), 1);
        QVERIFY(call.shareAudioApps().isEmpty());

        call.setShareAudioCanChooseAppsForTest(true);
        call.setShareAudioAppChosen(QStringLiteral("Firefox"),
                                    QStringLiteral("Firefox"), true);
        QCOMPARE(call.shareAudioMode(), 2);
        QCOMPARE(call.shareAudioApps(), QStringList{ QStringLiteral("firefox") });
        bool listed = false;
        for (const QVariant &row : call.shareAudioApplications()) {
            const QVariantMap m = row.toMap();
            if (m.value(QStringLiteral("key")).toString()
                == QLatin1String("firefox")) {
                listed = m.value(QStringLiteral("chosen")).toBool()
                    && m.value(QStringLiteral("label")).toString()
                        == QLatin1String("Firefox");
            }
        }
        QVERIFY2(listed, "a chosen application vanished from the list");
        call.setShareAudioEnabled(false);
        QCOMPARE(call.shareAudioMode(), 0);
        call.setShareAudioEnabled(true);
        QCOMPARE(call.shareAudioMode(), 2);
        QCOMPARE(call.shareAudioApps().size(), 1);
        call.chooseOnlyShareAudioApp(QStringLiteral("mpv.exe"),
                                     QStringLiteral("mpv"));
        QCOMPARE(call.shareAudioApps(), QStringList{ QStringLiteral("mpv.exe") });
        call.restoreShareAudioChoice(1, {});
        QCOMPARE(call.shareAudioMode(), 1);
        QVERIFY(call.shareAudioApps().isEmpty());
    }

    void aWindowPreselectedSoundChoiceEndsWithItsShare()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setShareAudioCanChooseAppsForTest(true);
        call.setCallStateForTest(SfuCallController::State::Connected);
        QCOMPARE(call.shareAudioMode(), 1);
        call.chooseOnlyShareAudioApp(QStringLiteral("vlc.exe"),
                                     QStringLiteral("VLC"));
        call.restoreShareAudioChoiceAfterShare(1, {});
        QCOMPARE(call.shareAudioMode(), 2);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("connection_lost"))));
        QCOMPARE(call.shareAudioMode(), 1);
        QVERIFY2(call.shareAudioApps().isEmpty(),
                 "the next share would silently carry only the last shared "
                 "window's application");
    }

    void aSoundChoiceMadeDuringTheShareOutlivesIt()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setShareAudioCanChooseAppsForTest(true);
        call.setCallStateForTest(SfuCallController::State::Connected);
        QCOMPARE(call.shareAudioMode(), 1);
        call.chooseOnlyShareAudioApp(QStringLiteral("vlc.exe"),
                                     QStringLiteral("VLC"));
        call.restoreShareAudioChoiceAfterShare(1, {});
        call.setShareAudioMode(0);
        QVERIFY(QMetaObject::invokeMethod(
            &call, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("connection_lost"))));
        QVERIFY2(call.shareAudioMode() == 0,
                 qPrintable(QStringLiteral(
                     "\"No sound\" chosen during the share became mode %1 "
                     "when it ended, so the next share carries sound")
                     .arg(call.shareAudioMode())));

        SfuCallController apps;
        apps.setClient(&client);
        apps.setShareAudioCanChooseAppsForTest(true);
        apps.setCallStateForTest(SfuCallController::State::Connected);
        apps.chooseOnlyShareAudioApp(QStringLiteral("vlc.exe"),
                                     QStringLiteral("VLC"));
        apps.restoreShareAudioChoiceAfterShare(1, {});
        apps.setShareAudioAppChosen(QStringLiteral("mpv.exe"),
                                    QStringLiteral("mpv"), true);
        QVERIFY(QMetaObject::invokeMethod(
            &apps, "onEngineFailed", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("connection_lost"))));
        QCOMPARE(apps.shareAudioMode(), 2);
        QCOMPARE(apps.shareAudioApps(),
                 (QStringList{ QStringLiteral("vlc.exe"),
                               QStringLiteral("mpv.exe") }));
    }

    void theShareSoundChoiceSurvivesARestart()
    {
        SettingsManager settings;
        settings.setCallShareAudioMode(1);
        {
            SfuCallController call;
            call.setSettings(&settings);
            QCOMPARE(call.shareAudioMode(), 1);
            call.setShareAudioMode(0);
        }
        QCOMPARE(settings.callShareAudioMode(), 0);
        {
            SfuCallController call;
            call.setSettings(&settings);
            QCOMPARE(call.shareAudioMode(), 0);
            QVERIFY(!call.shareAudioEnabled());
            call.setShareAudioMode(1);
        }
        SfuCallController back;
        back.setSettings(&settings);
        QCOMPARE(back.shareAudioMode(), 1);

        {
            SfuCallController call;
            call.setShareAudioCanChooseAppsForTest(true);
            call.setSettings(&settings);
            call.setShareAudioAppChosen(QStringLiteral("Firefox"),
                                        QStringLiteral("Firefox"), true);
            QCOMPARE(call.shareAudioMode(), 2);
        }
        SfuCallController withApps;
        withApps.setShareAudioCanChooseAppsForTest(true);
        withApps.setSettings(&settings);
        QCOMPARE(withApps.shareAudioMode(), 2);
        QCOMPARE(withApps.shareAudioApps(),
                 QStringList{ QStringLiteral("firefox") });
        QSettings().setValue(QStringLiteral("calls/shareAudioMode"), 7);
        QCOMPARE(settings.callShareAudioMode(), 1);
    }

    void uncheckingTheLastShareAudioAppSurvivesARestart()
    {
        {
            SettingsManager settings;
            settings.setCallShareAudioMode(1);
            settings.setCallShareAudioApps({});
            SfuCallController call;
            call.setShareAudioCanChooseAppsForTest(true);
            call.setSettings(&settings);
            call.setShareAudioAppChosen(QStringLiteral("firefox"),
                                        QStringLiteral("Firefox"), true);
            call.setShareAudioAppChosen(QStringLiteral("firefox"),
                                        QStringLiteral("Firefox"), false);
            QCOMPARE(call.shareAudioMode(), 2);
            QVERIFY(call.shareAudioApps().isEmpty());
            QCOMPARE(settings.callShareAudioMode(), 2);
            QVERIFY(settings.callShareAudioApps().isEmpty());
        }
        SettingsManager settings;
        SfuCallController restarted;
        restarted.setShareAudioCanChooseAppsForTest(true);
        restarted.setSettings(&settings);
        QCOMPARE(restarted.shareAudioMode(), 2);
        QVERIFY(restarted.shareAudioApps().isEmpty());
        QCOMPARE(settings.callShareAudioMode(), 2);
        QVERIFY(settings.callShareAudioApps().isEmpty());
    }

    void unavailablePerAppCaptureKeepsTheChoiceAcrossRestarts()
    {
        const QStringList apps{ QStringLiteral("firefox") };
        {
            SettingsManager settings;
            settings.setCallShareAudioMode(1);
            settings.setCallShareAudioApps({});
            SfuCallController call;
            call.setShareAudioCanChooseAppsForTest(true);
            call.setSettings(&settings);
            call.setShareAudioAppChosen(apps.first(),
                                        QStringLiteral("Firefox"), true);
        }
        {
            SettingsManager settings;
            SfuCallController restarted;
            restarted.setShareAudioCanChooseAppsForTest(false);
            restarted.setSettings(&settings);
            QCOMPARE(restarted.shareAudioMode(), 2);
            QCOMPARE(restarted.shareAudioApps(), apps);
            QCOMPARE(settings.callShareAudioMode(), 2);
            QCOMPARE(settings.callShareAudioApps(), apps);
            restarted.restoreShareAudioChoice(0, {});
            restarted.restoreShareAudioChoice(2, apps);
            QCOMPARE(restarted.shareAudioMode(), 2);
            QCOMPARE(restarted.shareAudioApps(), apps);
        }
        SettingsManager settings;
        SfuCallController availableAgain;
        availableAgain.setShareAudioCanChooseAppsForTest(true);
        availableAgain.setSettings(&settings);
        QCOMPARE(availableAgain.shareAudioMode(), 2);
        QCOMPARE(availableAgain.shareAudioApps(), apps);
    }

    void thePickersOneSharePreselectionIsNeverPersisted()
    {
        SettingsManager settings;
        settings.setCallShareAudioMode(1);
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setSettings(&settings);
        call.setShareAudioCanChooseAppsForTest(true);
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.chooseOnlyShareAudioApp(QStringLiteral("vlc.exe"),
                                     QStringLiteral("VLC"));
        QCOMPARE(call.shareAudioMode(), 2);
        QCOMPARE(settings.callShareAudioMode(), 1);
        QVERIFY(settings.callShareAudioApps().isEmpty());
        call.restoreShareAudioChoice(1, {});
        QCOMPARE(settings.callShareAudioMode(), 1);
    }

    void theShareSoundCanBeMutedOnItsOwnForOneShare()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        QVERIFY(!call.shareAudioMuted());
        QSignalSpy changed(&call, &SfuCallController::mediaStateChanged);
        QSignalSpy status(&call, &SfuCallController::shareAudioStatusChanged);
        call.setShareAudioMuted(true);
        QVERIFY(call.shareAudioMuted());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(status.count(), 1);
        QVERIFY2(!call.microphoneMuted(),
                 "muting the share's sound muted the microphone");
        call.setShareAudioMuted(true);
        QCOMPARE(changed.count(), 1);
        QVERIFY(QMetaObject::invokeMethod(&call, "toggleShareAudioMuted"));
        QVERIFY(!call.shareAudioMuted());
        call.setShareAudioMuted(true);

        const QMetaObject *meta = call.metaObject();
        const int index = meta->indexOfProperty("shareAudioMuted");
        QVERIFY(index >= 0);
        QVERIFY(meta->property(index).isWritable());
        QVERIFY(meta->property(index).hasNotifySignal());

        call.setCallStateForTest(SfuCallController::State::Connected);
        call.leave();
        QVERIFY2(!call.shareAudioMuted(),
                 "a mute made in one share carried into the next");

        using In = SfuCallController::ShareAudioStatusInput;
        In in;
        in.supported = true;
        in.mode = 1;
        in.muted = true;
        QVERIFY2(!SfuCallController::shareAudioStatusText(in).contains(
                     QStringLiteral("Muted")),
                 "a mute was reported with no share running");
        in.trackLive = true;
        const QString muted = SfuCallController::shareAudioStatusText(in);
        QVERIFY2(muted.contains(QStringLiteral("Muted")), qPrintable(muted));
        QVERIFY2(muted.contains(QStringLiteral("except Lightning")),
                 qPrintable(muted));
    }

    void shareAudioRebuildsAreCappedPerShare()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setClient(&client);
        call.setMembershipForTest(QStringLiteral("!share:x"), QString());
        call.setCallStateForTest(SfuCallController::State::Connected);
        call.setScreenSharingForTest(true);
        call.setShareAudioCidForTest(QStringLiteral("share-audio-1"));
        const auto ask = [&call](const QString &cid) {
            return QMetaObject::invokeMethod(&call, "onShareAudioRebuildWanted",
                                             Qt::DirectConnection,
                                             Q_ARG(QString, cid));
        };
        QVERIFY(ask(QStringLiteral("some-older-track")));
        QCOMPARE(call.shareAudioRebuildsForTest(), 0);
        for (int i = 0; i < 5; ++i)
            QVERIFY(ask(QStringLiteral("share-audio-1")));
        QCOMPARE(call.shareAudioRebuildsForTest(), 3);
        call.leave();
        QCOMPARE(call.shareAudioRebuildsForTest(), 0);
    }

    void theShareSoundStatusSaysWhatIsActuallySent()
    {
        using In = SfuCallController::ShareAudioStatusInput;
        In in;
        in.supported = false;
        using Why = SfuCallController::ShareAudioUnavailable;
        const QList<QPair<Why, QString>> reasons = {
            { Why::NoCapture, QStringLiteral("isn't available on this system") },
            { Why::MissingPlugin, QStringLiteral("lacks the audio capture "
                                                 "plugins") },
            { Why::NoPipeWire, QStringLiteral("needs PipeWire") },
            { Why::OldWindows, QStringLiteral("build 20348") },
        };
        QSet<QString> sentences;
        for (const auto &reason : reasons) {
            in.why = reason.first;
            const QString text = SfuCallController::shareAudioStatusText(in);
            QVERIFY2(text.contains(reason.second), qPrintable(text));
            QVERIFY2(!text.contains(QLatin1Char('\n')), qPrintable(text));
            sentences.insert(text);
        }
        QCOMPARE(sentences.size(), reasons.size());

        in.supported = true;
        in.mode = 0;
        QVERIFY(SfuCallController::shareAudioStatusText(in).contains(
            QStringLiteral("no sound")));

        in.mode = 1;
        const QString excluded = SfuCallController::shareAudioStatusText(in);
        QVERIFY2(excluded.contains(QStringLiteral("except Lightning")),
                 qPrintable(excluded));
        QVERIFY2(!excluded.contains(QStringLiteral("including this call")),
                 qPrintable(excluded));

        in.mode = 2;
        in.chosenLabels = {};
        QVERIFY(SfuCallController::shareAudioStatusText(in).contains(
            QStringLiteral("No apps chosen")));
        in.chosenLabels = { QStringLiteral("Firefox"), QStringLiteral("mpv") };
        const QString named = SfuCallController::shareAudioStatusText(in);
        QVERIFY2(named.contains(QStringLiteral("Firefox, mpv")),
                 qPrintable(named));

        in.trackLive = true;
        in.perApplication = true;
        in.carriedAny = false;
        QVERIFY(SfuCallController::shareAudioStatusText(in).contains(
            QStringLiteral("None of them is playing")));
        in.carriedAny = true;
        in.levelKnown = true;
        in.heard = false;
        QVERIFY(SfuCallController::shareAudioStatusText(in).contains(
            QStringLiteral("Nothing is being heard")));
        in.heard = true;
        QCOMPARE(SfuCallController::shareAudioStatusText(in), named);

        in.mode = 1;
        in.perApplication = false;
        in.heard = false;
        QVERIFY(SfuCallController::shareAudioStatusText(in).contains(
            QStringLiteral("Nothing is being heard")));

        in.heard = true;

        in.mode = 2;
        in.chosenLabels = { QStringLiteral("VLC") };
        in.perApplication = true;
        in.carriedAny = true;
        in.failedLabels = { QStringLiteral("VLC") };
        in.limitReached = true;
        const QString troubles = SfuCallController::shareAudioStatusText(in);
        QVERIFY2(troubles.contains(QStringLiteral("VLC couldn't be captured")),
                 qPrintable(troubles));
        QVERIFY2(troubles.contains(QStringLiteral("Too many apps")),
                 qPrintable(troubles));
    }

    void aSessionLostToTheNetworkRejoinsWithoutEndingTheCall()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setReconnectTimingForTest(20, 5000, 20000);
        connectRecoverableCall(client, call, QStringLiteral("delay-1"));
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        client.emitSfuState(QStringLiteral("reconnecting"),
                            QStringLiteral("server_leave"));
        QCOMPARE(call.state(), SfuCallController::State::Reconnecting);
        QVERIFY(call.active());
        QCOMPARE(call.roomId(), kReconnectRoom);
        QCOMPARE(failed.count(), 0);
        QVERIFY2(client.retractions.isEmpty(),
                 "the membership was retracted: to everyone else we left");

        call.startRefreshTickForTest(10);
        QTRY_VERIFY(!client.delayedRestarts.isEmpty());

        QTRY_COMPARE(client.sfuConnects.size(), 2);
        QCOMPARE(client.sfuConnects.last(),
                 qMakePair(kReconnectFocus, kReconnectRoom));
        QCOMPARE(call.reconnectAttemptsForTest(), 1);

        client.emitSfuState(QStringLiteral("authorized"), QString());
        client.emitSfuState(QStringLiteral("signalling"), QString());
        emitSfuJoin(client, QStringLiteral("@me:example.org:MEDEV"), {
            sfuParticipant(QStringLiteral("@me:example.org:MEDEV"),
                           QStringLiteral("PA_ME2"), {}),
            sfuParticipant(QStringLiteral("@b:example.org:BDEV"),
                           QStringLiteral("PA_B"), {}),
        });
        QVERIFY2(call.state() == SfuCallController::State::Reconnecting,
                 "a JoinResponse is not media: with the network still down "
                 "the websocket comes back while nothing can be heard");

        QVERIFY(emitTransport(call, 1, QStringLiteral("connected")));
        QCOMPARE(call.state(), SfuCallController::State::Connected);
        QCOMPARE(call.reconnectAttemptsForTest(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(client.retractions.isEmpty());
        QCOMPARE(call.roomId(), kReconnectRoom);

        QTest::qWait(150);
        QCOMPARE(client.sfuConnects.size(), 2);
        QCOMPARE(call.state(), SfuCallController::State::Connected);
    }

    void eachWayOfLosingTheSessionReconnectsButARefusalStillEnds()
    {
        const auto reconnects = [](const QString &state,
                                   const QString &category) {
            RecordingCallClient client;
            SfuCallController call;
            call.setReconnectTimingForTest(10000, 5000, 20000);
            connectRecoverableCall(client, call);
            client.emitSfuState(state, category);
            return call.state();
        };
        using S = SfuCallController::State;
        QCOMPARE(reconnects(QStringLiteral("reconnecting"),
                            QStringLiteral("signal_lost")), S::Reconnecting);
        QCOMPARE(reconnects(QStringLiteral("reconnecting"),
                            QStringLiteral("signal_timeout")), S::Reconnecting);
        QVERIFY2(reconnects(QStringLiteral("failed"),
                            QStringLiteral("connection_lost"))
                     == S::Reconnecting,
                 "a lost socket ended a connected call");
        QCOMPARE(reconnects(QStringLiteral("closed"), QString()),
                 S::Reconnecting);
        QCOMPARE(reconnects(QStringLiteral("failed"),
                            QStringLiteral("sfu_forbidden")), S::Failed);
        QCOMPARE(reconnects(QStringLiteral("ended"),
                            QStringLiteral("server_leave")), S::Failed);

        {
            RecordingCallClient client;
            SfuCallController call;
            call.setReconnectTimingForTest(10000, 5000, 20000);
            connectRecoverableCall(client, call);
            QVERIFY(emitTransport(call, 0, QStringLiteral("failed")));
            QCOMPARE(call.state(), S::Reconnecting);
        }
        {
            RecordingCallClient client;
            SfuCallController call;
            call.setReconnectTimingForTest(10000, 5000, 20000, 60);
            connectRecoverableCall(client, call);
            QVERIFY(emitTransport(call, 1, QStringLiteral("disconnected")));
            QCOMPARE(call.state(), S::Connected);
            QVERIFY(emitTransport(call, 1, QStringLiteral("connected")));
            QTest::qWait(150);
            QCOMPARE(call.state(), S::Connected);
            QVERIFY(emitTransport(call, 1, QStringLiteral("disconnected")));
            QTRY_COMPARE(call.state(), S::Reconnecting);
        }
        {
            RecordingCallClient client;
            SfuCallController call;
            connectRecoverableCall(client, call);
            call.setCallStateForTest(S::Connecting);
            client.emitSfuState(QStringLiteral("reconnecting"),
                                QStringLiteral("server_leave"));
            QCOMPARE(call.state(), S::Failed);
        }
    }

    void aReconnectThatNeverGetsBackEndsHonestly()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setReconnectTimingForTest(10, 60, 400);
        connectRecoverableCall(client, call);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        client.emitSfuState(QStringLiteral("reconnecting"),
                            QStringLiteral("signal_lost"));
        QCOMPARE(call.state(), SfuCallController::State::Reconnecting);
        QTRY_COMPARE(client.sfuConnects.size(), 2);
        client.emitSfuState(QStringLiteral("failed"),
                            QStringLiteral("connect_timeout"));
        QCOMPARE(call.state(), SfuCallController::State::Reconnecting);
        QTRY_COMPARE(client.sfuConnects.size(), 3);

        QTRY_COMPARE_WITH_TIMEOUT(call.state(),
                                  SfuCallController::State::Failed, 5000);
        QCOMPARE(call.lastError(),
                 QStringLiteral("The call ended because the connection was "
                                "lost."));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().first().toString(), call.lastError());
        QCOMPARE(client.retractions.size(), 1);
        QCOMPARE(client.retractions.first().first, kReconnectRoom);
        QVERIFY2(client.sfuConnects.size() <= 12,
                 "the attempts were not bounded by the backoff");

        const int attempts = client.sfuConnects.size();
        QTest::qWait(200);
        QCOMPARE(client.sfuConnects.size(), attempts);
        QCOMPARE(call.state(), SfuCallController::State::Failed);

        RecordingCallClient refusing;
        SfuCallController refused;
        refused.setReconnectTimingForTest(10, 5000, 20000);
        connectRecoverableCall(refusing, refused);
        refusing.emitSfuState(QStringLiteral("reconnecting"),
                              QStringLiteral("server_leave"));
        QTRY_COMPARE(refusing.sfuConnects.size(), 2);
        refusing.emitSfuState(QStringLiteral("failed"),
                              QStringLiteral("sfu_forbidden"));
        QCOMPARE(refused.state(), SfuCallController::State::Failed);
        QVERIFY(!refused.lastError().isEmpty());
        QVERIFY(refused.lastError() != call.lastError());
    }

    void leavingWhileReconnectingEndsAtOnce()
    {
        using S = SfuCallController::State;
        {
            RecordingCallClient client;
            SfuCallController call;
            call.setReconnectTimingForTest(40, 5000, 20000);
            connectRecoverableCall(client, call);
            client.emitSfuState(QStringLiteral("reconnecting"),
                                QStringLiteral("server_leave"));
            QCOMPARE(call.state(), S::Reconnecting);
            call.leave();
            QCOMPARE(call.state(), S::Ended);
            QCOMPARE(client.retractions.size(), 1);
            QTest::qWait(200);
            QVERIFY2(client.sfuConnects.size() == 1,
                     "an attempt was made after the user left");
        }
        {
            RecordingCallClient client;
            SfuCallController call;
            call.setReconnectTimingForTest(10, 5000, 20000);
            connectRecoverableCall(client, call);
            client.emitSfuState(QStringLiteral("reconnecting"),
                                QStringLiteral("server_leave"));
            QTRY_COMPARE(client.sfuConnects.size(), 2);
            call.leave();
            QCOMPARE(call.state(), S::Ended);
            client.emitSfuState(QStringLiteral("authorized"), QString());
            client.emitSfuState(QStringLiteral("signalling"), QString());
            emitSfuJoin(client, QStringLiteral("@me:example.org:MEDEV"), {});
            QVERIFY(emitTransport(call, 1, QStringLiteral("connected")));
            QCOMPARE(call.state(), S::Ended);
            QTest::qWait(150);
            QCOMPARE(client.sfuConnects.size(), 2);
        }
    }

    void aStaleReportFromTheAbandonedSessionChangesNothing()
    {
        using S = SfuCallController::State;
        RecordingCallClient client;
        SfuCallController call;
        call.setReconnectTimingForTest(10000, 5000, 60000);
        connectRecoverableCall(client, call);
        call.setOwnIdentityForTest(QStringLiteral("@me:example.org:MEDEV"));
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("@b:example.org:BDEV"),
                           QStringLiteral("PA_B"), {}),
        });
        const int rows = call.participantModel()->rowCount();
        QVERIFY(rows >= 1);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        client.emitSfuState(QStringLiteral("reconnecting"),
                            QStringLiteral("server_leave"));
        QCOMPARE(call.state(), S::Reconnecting);
        QCOMPARE(call.participantModel()->rowCount(), rows);

        client.emitSfuState(QStringLiteral("closed"), QString());
        client.emitSfuState(QStringLiteral("ended"),
                            QStringLiteral("server_leave"));
        client.emitSfuState(QStringLiteral("failed"),
                            QStringLiteral("connection_lost"));
        client.emitSfuState(QStringLiteral("failed"),
                            QStringLiteral("sfu_forbidden"));
        emitSfuJoin(client, QStringLiteral("@me:example.org:MEDEV"), {});
        QVariantMap gone = sfuParticipant(
            QStringLiteral("@b:example.org:BDEV"), QString(), {});
        gone.insert(QStringLiteral("state"), QStringLiteral("disconnected"));
        const QVariantList staleUpdate{ gone };
        QVERIFY(QMetaObject::invokeMethod(&call, "onSfuParticipants",
                                          Qt::DirectConnection,
                                          Q_ARG(QVariantList, staleUpdate)));
        QVERIFY(emitTransport(call, 1, QStringLiteral("connected")));

        QCOMPARE(call.state(), S::Reconnecting);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(call.reconnectAttemptsForTest(), 0);
        QCOMPARE(client.sfuConnects.size(), 1);
        QVERIFY2(call.participantModel()->rowCount() == rows,
                 "a participant update from the abandoned session reached "
                 "the call");
        QVERIFY(client.retractions.isEmpty());
    }

    void aRejoinSendsOurCurrentKeyAgainToMembersOnly()
    {
        KeyLaneCall c({ QStringLiteral("ADEV") });
        c.sfuJoin(QStringLiteral("ADEV"), QStringLiteral("PA_A1"));
        c.call.startKeyLaneForTest();
        QCOMPARE(c.client.keySends.size(), 1);
        const int key = c.newest();
        QCOMPARE(c.adopted(), key);

        c.call.setReconnectTimingForTest(10, 5000, 20000);
        const quint64 op = c.call.beginMembershipPublishForTest(
            c.room, kReconnectFocus);
        c.client.answerPublish(op, true, QString());
        c.call.setCallStateForTest(SfuCallController::State::Connected);

        c.client.emitSfuState(QStringLiteral("reconnecting"),
                              QStringLiteral("server_leave"));
        QCOMPARE(c.call.state(), SfuCallController::State::Reconnecting);
        QCOMPARE(c.client.keySends.size(), 1);
        QTRY_COMPARE(c.client.sfuConnects.size(), 2);
        c.client.emitSfuState(QStringLiteral("authorized"), QString());
        c.client.emitSfuState(QStringLiteral("signalling"), QString());
        emitSfuJoin(c.client, QStringLiteral("@me:example.org:MEDEV"), {
            sfuParticipant(QStringLiteral("@me:example.org:MEDEV"),
                           QStringLiteral("PA_ME2"), {}),
            sfuParticipant(KeyLaneCall::identityFor(QStringLiteral("ADEV")),
                           QStringLiteral("PA_A1"), {}),
            sfuParticipant(QStringLiteral("@stranger:example.org:XDEV"),
                           QStringLiteral("PA_X"), {}),
        });

        QCOMPARE(c.client.keySends.size(), 2);
        QCOMPARE(c.lastSend().devices, QStringList{ QStringLiteral("ADEV") });
        QCOMPARE(c.lastSend().index, key);
        QVERIFY(c.lastSend().key == c.client.keySends.at(0).key);
        QCOMPARE(c.newest(), key);
        QCOMPARE(c.adopted(), key);
    }

    void reconnectEpisodesAreCappedSoAFocusCannotCycleUsForEver()
    {
        using S = SfuCallController::State;
        RecordingCallClient client;
        SfuCallController call;
        call.setReconnectTimingForTest(10, 5000, 20000);
        call.setReconnectEpisodeCapForTest(2, 60000);
        connectRecoverableCall(client, call);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        const auto lostAndRecovered = [&client, &call] {
            const int connects = client.sfuConnects.size();
            client.emitSfuState(QStringLiteral("reconnecting"),
                                QStringLiteral("server_leave"));
            QCOMPARE(call.state(), S::Reconnecting);
            QTRY_COMPARE(client.sfuConnects.size(), connects + 1);
            client.emitSfuState(QStringLiteral("authorized"), QString());
            client.emitSfuState(QStringLiteral("signalling"), QString());
            emitSfuJoin(client, QStringLiteral("@me:example.org:MEDEV"), {});
            QVERIFY(emitTransport(call, 1, QStringLiteral("connected")));
            QCOMPARE(call.state(), S::Connected);
        };
        lostAndRecovered();
        lostAndRecovered();
        QCOMPARE(failed.count(), 0);

        client.emitSfuState(QStringLiteral("reconnecting"),
                            QStringLiteral("server_leave"));
        QVERIFY2(call.state() == S::Failed,
                 "a third loss inside the window started another reconnect");
        QCOMPARE(call.lastError(),
                 QStringLiteral("The call ended because the connection was "
                                "lost."));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(client.retractions.size(), 1);
        const int connects = client.sfuConnects.size();
        QTest::qWait(100);
        QCOMPARE(client.sfuConnects.size(), connects);

        RecordingCallClient later;
        SfuCallController rolling;
        rolling.setReconnectTimingForTest(10, 5000, 20000);
        rolling.setReconnectEpisodeCapForTest(1, 150);
        connectRecoverableCall(later, rolling);
        const auto recover = [&later, &rolling] {
            const int before = later.sfuConnects.size();
            later.emitSfuState(QStringLiteral("reconnecting"),
                               QStringLiteral("server_leave"));
            QCOMPARE(rolling.state(), S::Reconnecting);
            QTRY_COMPARE(later.sfuConnects.size(), before + 1);
            later.emitSfuState(QStringLiteral("authorized"), QString());
            later.emitSfuState(QStringLiteral("signalling"), QString());
            emitSfuJoin(later, QStringLiteral("@me:example.org:MEDEV"), {});
            QVERIFY(emitTransport(rolling, 1, QStringLiteral("connected")));
            QCOMPARE(rolling.state(), S::Connected);
        };
        recover();
        QTest::qWait(250);
        recover();
    }

    void aCallWhoseTransportNeverConnectedSaysItCouldNotConnect()
    {
        RecordingCallClient client;
        SfuCallController call;
        call.setReconnectTimingForTest(10, 60, 300);
        connectRecoverableCall(client, call, QString(), kReconnectRoom,
false);
        QSignalSpy failed(&call, &SfuCallController::callFailed);
        QVERIFY(emitTransport(call, 0, QStringLiteral("failed")));
        QCOMPARE(call.state(), SfuCallController::State::Reconnecting);
        QTRY_COMPARE_WITH_TIMEOUT(call.state(),
                                  SfuCallController::State::Failed, 5000);
        QCOMPARE(call.lastError(),
                 QStringLiteral("Couldn't connect to the call."));
        QCOMPARE(failed.count(), 1);

        RecordingCallClient other;
        SfuCallController ended;
        connectRecoverableCall(other, ended, QString(), kReconnectRoom,
false);
        other.emitSfuState(QStringLiteral("ended"),
                           QStringLiteral("server_leave"));
        QCOMPARE(ended.state(), SfuCallController::State::Failed);
        QCOMPARE(ended.lastError(),
                 QStringLiteral("Couldn't connect to the call."));
    }

    void aShareGrantedWhileReconnectingIsRefusedWithAReason()
    {
        RecordingCallClient client;
        SfuCallController call;
        ScreenCastPortal portal;
        call.setScreenCastPortal(&portal);
        call.setReconnectTimingForTest(10000, 5000, 20000);
        connectRecoverableCall(client, call);
        client.emitSfuState(QStringLiteral("reconnecting"),
                            QStringLiteral("server_leave"));
        QCOMPARE(call.state(), SfuCallController::State::Reconnecting);
        QSignalSpy failed(&call, &SfuCallController::callFailed);

        Q_EMIT portal.ready(7, -1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().first().toString(),
                 QStringLiteral("You can share your screen once the call has "
                                "reconnected."));
        QCOMPARE(call.state(), SfuCallController::State::Reconnecting);
        QVERIFY(!call.screenSharing());
    }

private:
    QTemporaryDir m_configHome;
};

QTEST_GUILESS_MAIN(CallControllerTest)
#include "CallControllerTest.moc"
