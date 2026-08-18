// 2026-08-18 voice-call signaling pipes: the MSC2746 state machine in
// CallController, driven with synthetic CallSignal observations through a
// recording client double. Media does not exist; the outbound path is
// exercised through placeCallWithOffer (the future media backend's entry)
// with a synthetic SDP.
#include <QtTest/QtTest>

#include <QDateTime>
#include <QSignalSpy>

#include "calls/CallController.h"
#include "calls/CallMediaBackend.h"
#include "calls/SdpStore.h"
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

    QStringList offerRequests;
    QStringList answerRequests;
    QStringList remoteAnswers;
    QStringList closed;
    QString lastRemoteOffer;
    QString lastRemoteAnswer;
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

} // namespace

class CallControllerTest : public QObject
{
    Q_OBJECT

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
};

QTEST_GUILESS_MAIN(CallControllerTest)
#include "CallControllerTest.moc"
