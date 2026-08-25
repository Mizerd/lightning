// 2026-08-18 voice-call signaling pipes: the MSC2746 state machine in
// CallController, driven with synthetic CallSignal observations through a
// recording client double. Media does not exist; the outbound path is
// exercised through placeCallWithOffer (the future media backend's entry)
// with a synthetic SDP.
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
#include "calls/SfuCallController.h"
#include "matrix/CallSignal.h"
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
        // Record that an answer was dispatched WITHOUT retaining the SDP —
        // mirrors production's no-echo rule; the test only needs the fact.
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

/// FakeMediaBackend plus real mute bookkeeping.
///
/// Separate from FakeMediaBackend on purpose: the base double deliberately
/// leaves `supportsMuteControl()` at its false default, so the "no mute
/// without an engine that implements it" case has something honest to test
/// against.
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

// ---------------------------------------------------------------------------
// 2026-08-26 Discord-style call stage: the DATA LAYER.
//
// Every assertion below drives SfuCallController through the same private
// merge helpers the SFU slots use, via the test seams — which exist because
// the stage could not be instantiated in a test at ALL before this round.
// ---------------------------------------------------------------------------

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
        // Ours is "zzz": theirs ("aaa") wins — we hang ours up with
        // "replaced" and ring theirs.
        {
            RecordingCallClient client;
            CallController calls;
            calls.setClient(&client);
            QVERIFY(calls.placeCallWithOffer(QStringLiteral("!r:x"),
                                             QStringLiteral("v=0 sdp")));
            // Force a known call id ordering by using the generated one:
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
        // PART 7 (legacy/MatrixRTC coexistence). One caller can announce a
        // single call on BOTH lanes: an m.rtc.notification and a legacy
        // m.call.invite. Their ids can never match — an RTC session is keyed
        // on the notification event id — so the re-delivery guard misses it,
        // and before the fix the invite fell into the busy branch and sent
        // m.call.reject. Two wrongs at once: the caller is told we declined
        // while we are actually ringing the user for exactly that person in
        // exactly that room, and the user may then answer a call the caller
        // has already abandoned.
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

        // Still one ring, and NOTHING on the wire.
        QCOMPARE(calls.state(), CallController::State::Ringing);
        QCOMPARE(calls.activeCallId(), QStringLiteral("$notify-1"));
        QVERIFY2(client.sent.isEmpty(),
                 "a second lane for the same call must not be rejected");
    }

    void aDifferentSenderInTheSameRoomIsStillRejectedAsBusy()
    {
        // The guard must be narrow. Varying ONLY the sender proves the
        // sender clause carries weight — the earlier version of this case
        // changed room AND sender together, so it passed even with the
        // sender comparison deleted.
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
        // ...and varying ONLY the room proves the room clause carries
        // weight too. The same person calling from another room is a
        // genuinely different call.
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
        // PART 11. Mute must reach the ENGINE (which stops publishing), and
        // deafen must not resurrect a microphone the user had deliberately
        // muted before deafening.
        RecordingCallClient client;
        CallController calls;
        MuteTrackingBackend backend;
        calls.setClient(&client);
        calls.setMediaBackend(&backend);
        QVERIFY(calls.muteControlAvailable());

        // Get to a live call so the intent has somewhere to land.
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

        // Deafening while already muted, then undeafening, must leave the
        // microphone MUTED — it was muted before, and coming back live would
        // publish someone who never asked to be heard.
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
        // The regression this pins: the intent used to be pushed to the
        // engine only from onMediaConnected, which arrives through a QUEUED
        // marshal — at least one event-loop turn AFTER RTP is already
        // flowing. A user who muted in a previous call therefore published
        // live audio, and a deafened user heard remote audio, for the
        // opening window of every subsequent call.
        //
        // So the assertion is deliberately made BEFORE deliverConnected().
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
        // Mute/deafen survives call-to-call by design, but an account change
        // clears everything else here, and a deafened state carried silently
        // into the next account is unhearable with no visible cause.
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
        // The seam's default implementation is a no-op, so an engine that
        // does not override it must not light up a working-looking control.
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

    // Review 2026-08-18 M1: the busy auto-reject is a remotely triggered
    // send with zero user interaction — it must be bounded.
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

    // Review 2026-08-18 M4: re-delivery of the live session's own invite
    // must be idempotent — the busy branch must not reject our own ring.
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

    // Review 2026-08-18 m3: an ignored sender must elicit NOTHING — no
    // ring, no state, and no reject either (a wire event would confirm we
    // are online during the ignore-propagation race window).
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

    // Review 2026-08-18 C2: a stale send-op result from an ENDED call must
    // neither end nor mutate the next call's session.
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
        // The FIRST call's invite send now reports failure.
        client.emitSendFinished(firstInviteOp, false,
                                QStringLiteral("network"));
        QCOMPARE(calls.state(), CallController::State::Inviting);
        QCOMPARE(failed.count(), 0);
    }

    // Review 2026-08-18 M2: the targeted-invite filter must be live in the
    // PRODUCTION wiring — own identity resolved from the client, no extra
    // setter required.
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

    // ── media-seam round (2026-08-18 round 2) ─────────────────────────

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

    // ── review-round corrections (2026-08-18 round 2) ─────────────────

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

        // Glare against a pending-offer call likewise retires ours with
        // no wire hangup (only the reject of... nothing here: theirs
        // wins, ours was never announced).
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

    // ── ICE candidates + TURN (round 3: the real engine's transport) ──

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
        // PRODUCTION order: AppController registers the engine BEFORE the
        // client. The pre-fetch must fire once the PAIR completes — this
        // exact ordering is what previously masked the first-call TURN
        // gap (review round 3 HIGH).
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

    // Round-3 recheck corrections.

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

        // The caller trickles WHILE we ring — the engine has no session
        // yet, so these must be buffered, not dropped (they used to be).
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
        // 40 candidates in one burst: the Rust side rejects >32 per event
        // WHOLE, so the flush must chunk (40 -> 32 + 8, or with the end
        // marker on completion, 32 + 9).
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

    // -----------------------------------------------------------------
    // The call stage's data layer (2026-08-26).
    //
    // WHAT EACH OF THESE WOULD REPORT ON THE UNFIXED TREE is stated per
    // test. Several would not COMPILE there, which is itself the finding:
    // there was no participant model, no share model and no stage state,
    // and no way to put a participant in front of the stage without a live
    // SFU. §16 records twice what happens when policy is only ever asserted
    // by reading the source (the row window shipped as a permanent no-op;
    // the rail drop could never group), so the seam came first.
    // -----------------------------------------------------------------

    void speakerUpdatesNeverResetTheParticipantModel()
    {
        // THE defect the whole model exists for. Participants used to be a
        // Q_INVOKABLE QVariantList re-invoked behind a hand-bumped tick and
        // bound to views as a JS array; onSfuSpeakers emitted
        // participantsChanged on every SpeakersChanged round, so a talking
        // participant reset the model — and with it destroyed every tile and
        // every VideoOutput — several times a second.
        //
        // UNFIXED TREE: does not compile (no participantModel). Structurally
        // it would fail anyway: the array reassignment IS the reset.
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
        // LiveKit's SpeakerInfo carries `level` (0..1) and rust/src/sfu.rs
        // has emitted it all along; onSfuSpeakers read `active` and dropped
        // it into a QHash<QString,bool>. One discarded field was the whole
        // reason a volume-reactive ring was impossible.
        //
        // UNFIXED TREE: there is no speakingLevel to read — the value never
        // left the JSON.
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
        // The degrade path, and the refusal that goes with it: a boolean
        // must NOT be turned into an amplitude. `speaking` is true so the
        // ring is drawn; `speakingLevel` stays 0.0 so it is drawn at its
        // minimum rather than at a size nobody measured.
        //
        // UNFIXED TREE: no level exists at all, so this case is
        // indistinguishable from the one above — which is exactly why the
        // distinction has to be pinned.
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
        // LiveKit sends the ACTIVE set, so absence is the stop signal.
        // Reading "absent" as "unchanged" leaves a ring stuck on.
        //
        // UNFIXED TREE: the old hash was cleared each round too, so this
        // half was already right — it is pinned because the rewrite could
        // easily have turned the level hash into a merge.
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
        // A participant update that changes a value must not look like a
        // join. UNFIXED TREE: every update rebuilt the whole array.
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
        // "make sure multiple users can screen share". The stage used to ask
        // `sharingPerson`, which looped the participants and RETURNED THE
        // FIRST match — a second simultaneous share had no id, no tile and
        // no affordance anywhere in the tree.
        //
        // UNFIXED TREE: does not compile (no shareModel), and structurally
        // there is nothing a second share could have been.
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
        // Distinct routing keys, so two surfaces can render at once without
        // the one-sink-per-track-key rule blanking either.
        QCOMPARE(shares->get(0).value(QStringLiteral("trackKey")).toString(),
                 QStringLiteral("TR_share_a"));
        QCOMPARE(shares->get(1).value(QStringLiteral("trackKey")).toString(),
                 QStringLiteral("TR_share_b"));
    }

    void aDismissedShareStaysLiveAndIsAlwaysReachableAgain()
    {
        // THE INVARIANT, and the maintainer's report: "if share is closed no
        // way to get it back". Dismissal applies to the SPOTLIGHT and never
        // to the share's existence.
        //
        // UNFIXED TREE: "Back to grid" wrote layoutMode = "grid";
        // effectiveLayout returned that verbatim and nothing anywhere ever
        // wrote "auto" or "spotlight" back, so the spotlight was unreachable
        // for the component's lifetime — and the grid's tiles never asked
        // for a screen track, so the share was not drawn at all. The only
        // recovery was navigating away and back, which destroys the Loader.
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
        // It fell through to the OTHER share rather than to nothing.
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_a"));
        QCOMPARE(shares->rowCount(), 2); // still live, still a grid tile

        stage->dismissShare(QStringLiteral("TR_share_a"));
        QCOMPARE(stage->spotlightShareId(), QString());
        // ...and even with NOTHING on the spotlight the shares are still
        // rows, and the surface still has something to bind a "show it
        // again" control to. This is the machine-checkable statement of
        // "there is always a way back".
        QCOMPARE(shares->rowCount(), 2);
        QCOMPARE(stage->restorableShareAvailable(), true);
        QCOMPARE(stage->dismissedShareCount(), 2);

        stage->restoreAllShares();
        QCOMPARE(stage->spotlightShareId(), QStringLiteral("TR_share_b"));
        QCOMPARE(stage->restorableShareAvailable(), false);
    }

    void aNewShareReArmsTheSpotlightAfterTheUserChoseGrid()
    {
        // The layout preference must not LATCH. A share that starts after
        // the user pressed grid is not the thing they waved away.
        //
        // UNFIXED TREE: "grid" was absolute and permanent.
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
        // An unrecognised mode read back verbatim is precisely how the old
        // latch behaved. UNFIXED TREE: layoutMode was a bare string property
        // that accepted anything.
        SfuCallController call;
        CallStageState *stage = call.stageState();
        stage->setLayoutPreference(QStringLiteral("spotlight"));
        stage->setLayoutPreference(QStringLiteral("nonsense"));
        QCOMPARE(stage->layoutPreference(), QStringLiteral("spotlight"));
    }

    void aRestartedShareIsOfferedAgainRatherThanInheritingADismissal()
    {
        // A share that stops and starts is a NEW published track and so a
        // new sid. Inheriting the dismissal would leave the user with
        // nothing on screen and no explanation.
        //
        // UNFIXED TREE: does not compile; there was no per-share identity to
        // key anything on.
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

    void handRaiseIsLocalOnlyAndSaysSoOnEveryOtherRow()
    {
        // Nothing carries a raised hand on the wire: setHandRaised writes a
        // member and emits mediaStateChanged, and `hand` appears nowhere in
        // the media engine or the Rust call bridge. The role is kept
        // honestly rather than dropped, because the LOCAL badge is genuine
        // feedback; a remote one could never light.
        //
        // UNFIXED TREE: CallParticipantTile declared `handRaised` and
        // CallStage never bound it, so the badge could not light for anyone
        // at all — including the local user who had just pressed the button.
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
        // setParticipantVolume was write-only, which is why no QML ever
        // called it: a slider with nothing to bind to cannot show the value
        // it just set.
        //
        // UNFIXED TREE: there is no getter, no property and no role.
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
        // Clamped, not trusted.
        call.setParticipantVolume(QStringLiteral("alice"), 400);
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::VolumePercentRole)
                     .toInt(),
                 100);
    }

    void connectionQualityIsMergedAndUnknownIsNeverRendered()
    {
        // sfuConnectionQuality has been emitted by the bridge since the
        // interop round and connected to NOBODY.
        //
        // UNFIXED TREE: the signal has no receiver, so the role would be
        // permanently empty.
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

        // A round that does not mention a sid is a DELTA: the last known
        // value survives it.
        call.ingestConnectionQualityForTest({ unknown });
        QCOMPARE(participantRole(model, 0,
                                 CallParticipantModel::ConnectionQualityRole)
                     .toString(),
                 QStringLiteral("excellent"));
    }

    void leavingClearsTheStageStateSoTheNextCallStartsClean()
    {
        // The stage's view state lives in C++ precisely so it survives the
        // QML Loader a room switch destroys — which means nothing else
        // clears it, so leaving must.
        //
        // UNFIXED TREE: the state lived in the component, and the Loader
        // being destroyed was the ONLY thing that ever reset it. That
        // accident was also the only escape from the dismissed-share dead
        // end.
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
        // ONE derivation. The legacy invokable is kept for the surfaces that
        // still read it, but it can no longer disagree with the tiles.
        //
        // UNFIXED TREE: participants() rebuilt its own list from the SFU
        // payload — there was no second derivation to disagree WITH, which
        // is the point: this pins that the new one did not create one.
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

    // -----------------------------------------------------------------
    // 2026-08-27 regression round. Two reports, one cause, plus the
    // full-screen feature's one dangerous state.
    // -----------------------------------------------------------------

    void theParticipantCountNotifiesOnEveryPathThatMovesIt()
    {
        // `participantCount` was changed to read the MODEL's rowCount while
        // keeping `participantsChanged` as its NOTIFY — and the model is
        // rebuilt from paths that never emit it (`onSfuJoined`, and the
        // `mediaStateChanged` rebuild that runs on every mute, camera and
        // share change). So a surface binding the count saw a stale number.
        //
        // Asserted through the property's OWN notify signal rather than a
        // named one, so this compiles against both trees and measures the
        // contract rather than the implementation.
        //
        // UNFIXED TREE: FAILS. setOwnIdentityForTest() calls rebuildModels()
        // directly; the local placeholder row appears, the count goes 0 -> 1,
        // and nothing on that path emits participantsChanged — so the spy
        // counts zero.
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
        // THE one state this feature must never reach: a window filling the
        // monitor with an empty rectangle. The guard lives in the state
        // object rather than in a QML binding so every caller inherits it —
        // and so it can be driven, which a binding cannot.
        //
        // UNFIXED TREE: does not compile; there was no full-screen mode.
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

        // A PIN is equally something to show, with no share at all.
        stage->setFullScreen(false);
        stage->dismissShare(QStringLiteral("TR_share_a"));
        QCOMPARE(stage->spotlightShareId(), QString());
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), false); // nothing focused yet
        stage->pin(QStringLiteral("alice"));
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);
    }

    void fullScreenDropsItselfWhenTheFocusedSurfaceGoesAway()
    {
        // The share ends, or the user unpins, while full screen is up. The
        // flag has to fall on its own: the QML window is driven from it, and
        // a stale true would leave a black monitor over the desktop with the
        // call's own controls as the only clue.
        //
        // UNFIXED TREE: does not compile.
        SfuCallController call;
        call.ingestParticipantsForTest({
            sfuParticipant(QStringLiteral("alice"), QStringLiteral("PA_1"),
                           { sfuTrack(QStringLiteral("screen_share"),
                                      QStringLiteral("TR_share_a"), false) }),
        });
        CallStageState *stage = call.stageState();
        stage->setFullScreen(true);
        QCOMPARE(stage->fullScreen(), true);

        // The sharer stops sharing: the share row goes, and with it the only
        // thing full screen was showing.
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
};


QTEST_GUILESS_MAIN(CallControllerTest)
#include "CallControllerTest.moc"
