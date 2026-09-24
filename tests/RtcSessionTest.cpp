// RtcController's MatrixRTC session store: poke coalescing, generation
// isolation and join-block reasons.
//
//  * A participant count comes only from a reply we asked for, for that room,
//    under the current account; a late reply from a previous account must not
//    write into the current one.
//  * A burst of pokes collapses into one read without losing a change that
//    arrived while a read was in flight.
//  * `joinBlockReason` keeps "not looked yet", "server has nothing" and "the
//    look failed" apart, since the UI wording differs.
//  * The same person on two devices is two participants but one face.
#include "calls/RtcController.h"
#include "matrix/MatrixClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");
const QString kOther = QStringLiteral("!other:example.org");

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    bool rtcSupported = true;
    bool refuseOps = false;
    QStringList sessionReads;
    QList<bool> sessionReadPreferredServer;
    QStringList transportRooms;
    quint64 lastSessionOp = 0;
    quint64 lastTransportsOp = 0;

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override
    { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override
    { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override
    { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override
    { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsMatrixRtc() const override { return rtcSupported; }
    quint64 rtcSession(const QString &roomId, bool preferServer) override
    {
        if (!rtcSupported || refuseOps)
            return 0;
        sessionReads.append(roomId);
        sessionReadPreferredServer.append(preferServer);
        lastSessionOp = nextOp++;
        return lastSessionOp;
    }
    quint64 rtcTransports(const QString &roomId) override
    {
        if (!rtcSupported || refuseOps)
            return 0;
        transportRooms.append(roomId);
        lastTransportsOp = nextOp++;
        return lastTransportsOp;
    }
};

RtcParticipant person(const QString &user, const QString &device,
                      qint64 joined = 1000, bool ownUser = false,
                      bool ownDevice = false)
{
    RtcParticipant p;
    p.userId = user;
    p.deviceId = device;
    p.rtcIdentity = user + QLatin1Char(':') + device;
    p.intent = QStringLiteral("audio");
    p.joinedAtMs = joined;
    p.expiresAtMs = joined + 3600000;
    p.wireFormat = QStringLiteral("session");
    p.ownUser = ownUser;
    p.ownDevice = ownDevice;
    return p;
}

RtcSessionData sessionFor(const QString &roomId,
                          const QVector<RtcParticipant> &people)
{
    RtcSessionData data;
    data.roomId = roomId;
    data.participants = people;
    return data;
}

} // namespace

class RtcSessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void reportsParticipantsFromAReplyWeAskedFor();
    void ignoresAReplyWeNeverAskedFor();
    void ignoresAReplyNamingADifferentRoom();
    void aReplyFromAPreviousAccountNeverLandsInTheNewOne();
    void signOutForgetsObservedCalls();
    void pokeBurstCollapsesIntoOneRead();
    void aPokeDuringAnInFlightReadIsNotLost();
    void unchangedSessionDoesNotAnnounceAChange();
    void slotClosedHidesTheSessionEntirely();
    void ownDeviceIsDistinctFromOwnUser();
    void oneFacePerPersonAcrossDevices();
    void joinBlockKeepsItsCausesApart();
    void unsupportedBackendDoesNothingAtAll();
    void availabilityNeedsAPositiveTransport();
    void anUnansweredReadReleasesItsRoom();
    void aReplacedClientsLateReplyCannotBeDelivered();
    void oneRoomsFocusDoesNotDecideAnothers();
    void anEncryptedRoomRefusesWithoutMediaEncryption();
    void aSessionThatNamesAFocusIsJoinableWithoutDiscovery();
    void anExistingSessionsFocusOutranksOurOwnHomeserver();
    void mediaKeyTargetsAddressEveryOtherDeviceAndNotOurOwn();
    void mediaKeyTargetsAreEmptyForARoomWithNoSession();
    void anOrdinaryRefreshTrustsTheLocalStore();
    void aForcedRefreshAsksTheHomeserver();
    void aForcedRefreshIsRateLimitedPerRoom();
    void aForcedRefreshDuringAnInFlightReadIsNotLost();
    void aNewAccountMayForceAReadImmediately();
    void aForcedReadThatChangesNothingBacksOff();
    void aForcedReadThatFoundSomebodyRestoresFullSpeed();
};

void RtcSessionTest::reportsParticipantsFromAReplyWeAskedFor()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    QSignalSpy changed(&controller, &RtcController::sessionChanged);

    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads, QStringList{kRoom});

    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@a:example.org"),
                                  QStringLiteral("D1"))}));

    QCOMPARE(controller.participantCount(kRoom), 1);
    QVERIFY(controller.hasLiveSession(kRoom));
    QCOMPARE(changed.count(), 1);
}

void RtcSessionTest::ignoresAReplyWeNeverAskedFor()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);

    // An op id nobody dispatched: accepting it would let a stray event install
    // a participant list.
    Q_EMIT client.rtcSessionReceived(
        4242, sessionFor(kRoom, {person(QStringLiteral("@a:example.org"),
                                        QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 0);
}

void RtcSessionTest::ignoresAReplyNamingADifferentRoom()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.refresh(kRoom);

    // Right op id, wrong room: would attribute one room's call to another.
    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kOther, {person(QStringLiteral("@a:example.org"),
                                   QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 0);
    QCOMPARE(controller.participantCount(kOther), 0);
}

void RtcSessionTest::aReplyFromAPreviousAccountNeverLandsInTheNewOne()
{
    FakeClient first;
    FakeClient second;
    RtcController controller;
    controller.setClient(&first);
    controller.refresh(kRoom);
    const quint64 staleOp = first.lastSessionOp;

    // Account switch: the in-flight read belongs to the outgoing account.
    controller.setClient(&second);

    Q_EMIT first.rtcSessionReceived(
        staleOp, sessionFor(kRoom, {person(QStringLiteral("@a:example.org"),
                                           QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 0);
}

void RtcSessionTest::signOutForgetsObservedCalls()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@a:example.org"),
                                  QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 1);

    // A participant list does not survive sign-out.
    client.logout();
    QCOMPARE(controller.participantCount(kRoom), 0);
}

void RtcSessionTest::pokeBurstCollapsesIntoOneRead()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(20);

    // A call filling up rewrites one state event per joiner; read once.
    for (int i = 0; i < 8; ++i)
        Q_EMIT client.rtcSessionChanged(kRoom);
    QCOMPARE(client.sessionReads.count(), 0); // nothing yet — still coalescing

    QTRY_COMPARE(client.sessionReads.count(), 1);
    QCOMPARE(client.sessionReads.first(), kRoom);
}

void RtcSessionTest::aPokeDuringAnInFlightReadIsNotLost()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(10);

    controller.refresh(kRoom); // read in flight, not yet answered
    QCOMPARE(client.sessionReads.count(), 1);

    // A change arrives while a read is outstanding; that reply predates it,
    // so dropping the poke would leave the banner stale.
    Q_EMIT client.rtcSessionChanged(kRoom);
    QTest::qWait(40);
    QCOMPARE(client.sessionReads.count(), 1); // still blocked, correctly

    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    QTRY_COMPARE(client.sessionReads.count(), 2);
}

void RtcSessionTest::unchangedSessionDoesNotAnnounceAChange()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    const auto people = QVector<RtcParticipant>{
        person(QStringLiteral("@a:example.org"), QStringLiteral("D1"))};

    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, people));
    QSignalSpy changed(&controller, &RtcController::sessionChanged);

    // Re-reading identical state does not re-render every banner and
    // facepile.
    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, people));
    QCOMPARE(changed.count(), 0);
}

void RtcSessionTest::slotClosedHidesTheSessionEntirely()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.refresh(kRoom);

    RtcSessionData data =
        sessionFor(kRoom, {person(QStringLiteral("@a:example.org"),
                                  QStringLiteral("D1"))});
    data.slotPresent = true;
    data.slotClosed = true;
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp, data);

    // An explicitly closed slot means the call is over despite lingering
    // memberships.
    QCOMPARE(controller.participantCount(kRoom), 0);
    QVERIFY(!controller.hasLiveSession(kRoom));
    QVERIFY(controller.participants(kRoom).isEmpty());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("session_closed"));
}

void RtcSessionTest::ownDeviceIsDistinctFromOwnUser()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.refresh(kRoom);

    // The local account is in the call from a different device, so "am I in
    // this call?" is no for this device.
    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@me:example.org"),
                                  QStringLiteral("OTHER"), 1000,
                                  /*ownUser=*/true, /*ownDevice=*/false)}));
    QVERIFY(controller.ownUserInSession(kRoom));
    QVERIFY(!controller.ownDeviceInSession(kRoom));
}

void RtcSessionTest::oneFacePerPersonAcrossDevices()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.refresh(kRoom);

    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom,
                   {person(QStringLiteral("@a:example.org"),
                           QStringLiteral("LAPTOP"), 1000),
                    person(QStringLiteral("@a:example.org"),
                           QStringLiteral("PHONE"), 2000),
                    person(QStringLiteral("@b:example.org"),
                           QStringLiteral("D1"), 3000)}));

    // Three participant devices...
    QCOMPARE(controller.participantCount(kRoom), 3);
    // ...but two people, and two faces.
    QCOMPARE(controller.participantUserIds(kRoom).count(), 2);
    QCOMPARE(controller.participantFaces(kRoom).count(), 2);
}

void RtcSessionTest::joinBlockKeepsItsCausesApart()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    // Transport causes only: mark the room unencrypted so the encryption gate
    // does not mask them.
    controller.setRoomEncrypted(kRoom, false);

    // Nothing looked yet: "checking", not "unavailable".
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("undiscovered"));
    QVERIFY(controller.discoveryWorthRetrying());

    // The server answered and named nothing: this homeserver has no
    // MatrixRTC, worded differently from a failed check. (answered = true,
    // category = "unsupported") is what Rust sends for a definitive 404; see
    // rtc.rs discovery_answer_is_definitive.
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(client.lastTransportsOp, true,
                                        QStringLiteral("unsupported"), {},
                                        QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_transport"));
    QVERIFY(!controller.callingAvailable());
    // A server that answered stops the automatic re-check
    // (discoveryWorthRetrying()), which would otherwise run on every room
    // change.
    QVERIFY2(!controller.discoveryWorthRetrying(),
             "a homeserver that answered \"no MatrixRTC here\" is still "
             "re-asked on every room change");

    // The check itself failed: unknown, not "no calling", and worth retrying.
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(client.lastTransportsOp, false,
                                        QStringLiteral("network"), {},
                                        QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("discovery_failed"));
    QVERIFY(controller.discoveryWorthRetrying());

    // A transport exists; the remaining blocker is this build's missing media
    // transport, a different sentence from "your server has no calling".
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    // Without an SFU media engine, joining would publish a membership no peer
    // could connect to.
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    QVERIFY(controller.callingAvailable());

    // With an engine the call is joinable; this is the only path to an empty
    // reason.
    controller.setMediaAvailable(true);
    QVERIFY(controller.joinBlockReason(kRoom).isEmpty());
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);

    // The room's power levels: with default levels an ordinary member cannot
    // write the call membership, so joining is blocked up front with its own
    // reason.
    controller.setCanPublishMembership(kRoom, false);
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::NoPermission);
    QCOMPARE(controller.joinBlockReason(kRoom), QStringLiteral("no_permission"));

    // Unknown capability is not a refusal: the button stays live and the
    // server decides (unlike the encryption gate, which fails safe by
    // refusing).
    controller.setCanPublishMembership(kRoom, true);
    QVERIFY(controller.joinBlockReason(kRoom).isEmpty());
    RtcController fresh;
    fresh.setClient(&client);
    fresh.setMediaAvailable(true);
    fresh.setRoomEncrypted(kRoom, false);
    fresh.refresh(kRoom);
    fresh.discover(kRoom);
    Q_EMIT client.rtcSessionChanged(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    QVERIFY2(fresh.joinBlockReason(kRoom) != QStringLiteral("no_permission"),
             "a room whose call-membership capability was never reported was "
             "refused, so a Join button was disabled on a guess");
}

void RtcSessionTest::unsupportedBackendDoesNothingAtAll()
{
    FakeClient client;
    client.rtcSupported = false;
    RtcController controller;
    controller.setClient(&client);

    controller.refresh(kRoom);
    controller.discover(kRoom);
    Q_EMIT client.rtcSessionChanged(kRoom);

    // No reads, no discovery, and an honest reason: mock and HTTP backends do
    // not appear to support calling.
    QVERIFY(client.sessionReads.isEmpty());
    QVERIFY(client.transportRooms.isEmpty());
    QVERIFY(!controller.supported());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("unsupported"));
}

void RtcSessionTest::availabilityNeedsAPositiveTransport()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);

    // Availability is a positive fact: false before any answer, and a
    // participant-advertised focus alone makes it true (servers without the
    // MSC4143 endpoint still support calling).
    QVERIFY(!controller.callingAvailable());
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(), {},
        QStringLiteral("https://peer-focus.example.org/"));
    QVERIFY(controller.callingAvailable());
    QVERIFY(controller.availabilityCategory().isEmpty());
}

void RtcSessionTest::anUnansweredReadReleasesItsRoom()
{
    // The Rust event queue drops the oldest event on overflow, so a reply may
    // never arrive; the read times out rather than leaving the room
    // permanently un-refreshable.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setReadTimeoutMsForTest(30);

    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    // No reply ever arrives; a second read is suppressed...
    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    // ...until the read times out.
    QTest::qWait(60);
    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 2);
}

void RtcSessionTest::aReplacedClientsLateReplyCannotBeDelivered()
{
    // Op ids come from a per-client counter, so a replacement client's ids
    // collide with the old one's. setClient disconnects the previous client,
    // so its late reply is never delivered (see RtcController.h).
    FakeClient first;
    FakeClient second;
    RtcController controller;
    controller.setClient(&first);
    controller.refresh(kRoom);
    const quint64 staleOp = first.lastSessionOp;

    controller.setClient(&second);
    // The new client hands out the same op id for its first read.
    controller.refresh(kRoom);
    QCOMPARE(second.lastSessionOp, staleOp);

    // The old client's reply with that id is refused.
    Q_EMIT first.rtcSessionReceived(
        staleOp, sessionFor(kRoom, {person(QStringLiteral("@ghost:example.org"),
                                           QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 0);

    // The new client's reply for the same id is accepted.
    Q_EMIT second.rtcSessionReceived(
        second.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@real:example.org"),
                                  QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 1);
}

void RtcSessionTest::oneRoomsFocusDoesNotDecideAnothers()
{
    // A focus advertised by room A's participants says nothing about room B.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    // Both rooms unencrypted, so the encryption gate does not mask the
    // transport answer.
    controller.setRoomEncrypted(kRoom, false);
    controller.setRoomEncrypted(kOther, false);

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(), {},
        QStringLiteral("https://room-a-focus.example.org/"));

    // Room A has a reachable transport...
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    // ...room B does not, and says so.
    QCOMPARE(controller.joinBlockReason(kOther),
             QStringLiteral("no_transport"));
}

void RtcSessionTest::aSessionThatNamesAFocusIsJoinableWithoutDiscovery()
{
    // A session whose memberships name a focus is a reachable transport,
    // regardless of discovery: most homeservers lack the MSC4143 endpoint, and
    // joining must still work where the call is visible.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setRoomEncrypted(kRoom, false);
    controller.setMediaAvailable(true);
    controller.setMediaEncryptionAvailable(true);

    // Nothing discovered, and nothing ever will be (no MSC4143 here).
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("undiscovered"));

    controller.refresh(kRoom);
    RtcSessionData session =
        sessionFor(kRoom, {person(QStringLiteral("@a:example.org"),
                                  QStringLiteral("AAA"), 1000)});
    session.focusServiceUrl = QStringLiteral("https://sfu.example.org/");
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp, session);

    QVERIFY2(controller.joinBlockReason(kRoom).isEmpty(),
             qPrintable(QStringLiteral("still blocked: %1")
                            .arg(controller.joinBlockReason(kRoom))));
    QCOMPARE(controller.focusUrlFor(kRoom),
             QStringLiteral("https://sfu.example.org/"));
    // It counts as availability without a discovery round trip.
    QVERIFY(controller.callingAvailable());

    // A failed discovery does not undo it.
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(client.lastTransportsOp, false,
                                        QStringLiteral("network"), {},
                                        QString());
    QVERIFY2(controller.joinBlockReason(kRoom).isEmpty(),
             qPrintable(QStringLiteral("a failed discovery re-blocked a "
                                       "session that names a focus: %1")
                            .arg(controller.joinBlockReason(kRoom))));
}

void RtcSessionTest::anExistingSessionsFocusOutranksOurOwnHomeserver()
{
    // With a call running, use the focus the oldest membership named, not our
    // own homeserver's SFU, so everyone ends up in one call (as the reference
    // implementation does).
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setRoomEncrypted(kRoom, false);

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://ours.example.org/")}, QString());
    // With no session, our own server is right: starting a call.
    QCOMPARE(controller.focusUrlFor(kRoom),
             QStringLiteral("https://ours.example.org/"));

    controller.refresh(kRoom);
    RtcSessionData session =
        sessionFor(kRoom, {person(QStringLiteral("@a:example.org"),
                                  QStringLiteral("AAA"), 1000)});
    session.focusServiceUrl = QStringLiteral("https://theirs.example.org/");
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp, session);

    QCOMPARE(controller.focusUrlFor(kRoom),
             QStringLiteral("https://theirs.example.org/"));

    // A closed slot is not a session to agree with.
    RtcSessionData closed = session;
    closed.slotClosed = true;
    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp, closed);
    QCOMPARE(controller.focusUrlFor(kRoom),
             QStringLiteral("https://ours.example.org/"));
}

void RtcSessionTest::mediaKeyTargetsAddressEveryOtherDeviceAndNotOurOwn()
{
    // Media key targets are per device (Olm to-device): every other device
    // needs one, and our own device does not.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom,
                   {person(QStringLiteral("@me:example.org"),
                           QStringLiteral("MINE"), 1000,
                           /*ownUser=*/true, /*ownDevice=*/true),
                    person(QStringLiteral("@me:example.org"),
                           QStringLiteral("OTHER"), 1100,
                           /*ownUser=*/true, /*ownDevice=*/false),
                    person(QStringLiteral("@them:example.org"),
                           QStringLiteral("THEIRS"), 1200,
                           /*ownUser=*/false, /*ownDevice=*/false)}));

    const QJsonArray targets =
        QJsonDocument::fromJson(
            controller.mediaKeyTargetsJson(kRoom).toUtf8()).array();
    QCOMPARE(targets.size(), 2);

    // Our own device is absent; our account's second device is present, since
    // it is a separate Olm session.
    QSet<QString> pairs;
    for (const QJsonValue &value : targets) {
        pairs.insert(value.toObject().value(QStringLiteral("user_id"))
                         .toString()
                     + QLatin1Char('/')
                     + value.toObject().value(QStringLiteral("device_id"))
                           .toString());
    }
    QVERIFY(!pairs.contains(QStringLiteral("@me:example.org/MINE")));
    QVERIFY(pairs.contains(QStringLiteral("@me:example.org/OTHER")));
    QVERIFY(pairs.contains(QStringLiteral("@them:example.org/THEIRS")));
}

void RtcSessionTest::mediaKeyTargetsAreEmptyForARoomWithNoSession()
{
    // An empty target list is still valid JSON: the Rust side fails the whole
    // send on invalid JSON.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    QCOMPARE(controller.mediaKeyTargetsJson(kRoom), QStringLiteral("[]"));
    const QJsonDocument parsed = QJsonDocument::fromJson(
        controller.mediaKeyTargetsJson(kOther).toUtf8());
    QVERIFY(parsed.isArray());
    QVERIFY(parsed.array().isEmpty());
}

void RtcSessionTest::anEncryptedRoomRefusesWithoutMediaEncryption()
{
    // A call in an end-to-end encrypted room fails safely rather than
    // publishing media the SFU can read.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());

    // Explicitly unencrypted room: the blocker is the missing transport.
    controller.setRoomEncrypted(kRoom, false);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));

    // Encrypted room without media E2EE: a different, more important refusal,
    // and the one reported.
    controller.setRoomEncrypted(kRoom, true);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("media_encryption_unavailable"));

    // With media E2EE active only the transport blocker remains.
    controller.setMediaEncryptionAvailable(true);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    // ...and with an engine an encrypted room is joinable.
    controller.setMediaAvailable(true);
    QVERIFY(controller.joinBlockReason(kRoom).isEmpty());

    // A room never reported defaults to encrypted: a boolean cannot say
    // "unknown", and assuming unencrypted would be a silent downgrade.
    RtcController fresh;
    FakeClient freshClient;
    fresh.setClient(&freshClient);
    fresh.discover(kOther);
    Q_EMIT freshClient.rtcTransportsReceived(
        freshClient.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    QCOMPARE(fresh.joinBlockReason(kOther),
             QStringLiteral("media_encryption_unavailable"));
    // Encryption is checked first, even with media available.
    fresh.setMediaAvailable(true);
    QCOMPARE(fresh.joinBlockReason(kOther),
             QStringLiteral("media_encryption_unavailable"));
}


// Server-backed reads. The SFU only lists participants who authenticated as
// real Matrix identities, so one that no membership accounts for means our
// local state is incomplete; a forced /state read is the way back.

void RtcSessionTest::anOrdinaryRefreshTrustsTheLocalStore()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);

    controller.refresh(kRoom);
    QCOMPARE(client.sessionReadPreferredServer, QList<bool>{false});
}

void RtcSessionTest::aForcedRefreshAsksTheHomeserver()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);

    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads, QStringList{kRoom});
    QCOMPARE(client.sessionReadPreferredServer, QList<bool>{true});
}

void RtcSessionTest::aForcedRefreshIsRateLimitedPerRoom()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setServerReadCooldownMsForTest(60000);

    // The trigger fires in bursts; one /state request per burst.
    controller.refreshFromServer(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    controller.refreshFromServer(kRoom);
    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    // ...and a different room is a separate question.
    controller.refreshFromServer(QStringLiteral("!other:example.org"));
    QCOMPARE(client.sessionReads.count(), 2);
    QCOMPARE(client.sessionReadPreferredServer.last(), true);

    // Once the window passes, the room may be asked again.
    controller.setServerReadCooldownMsForTest(0);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(QStringLiteral("!other:example.org"), {}));
    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 3);
    QCOMPARE(client.sessionReadPreferredServer.last(), true);
}

void RtcSessionTest::aForcedRefreshDuringAnInFlightReadIsNotLost()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(10);

    // A store-backed read is outstanding, but its reply carries the answer
    // that prompted the forced read, so the forced read is not folded into it.
    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);
    QCOMPARE(client.sessionReadPreferredServer.first(), false);

    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 1); // still only the first

    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    QTRY_COMPARE(client.sessionReads.count(), 2);
    QCOMPARE(client.sessionReadPreferredServer.last(), true);
}

void RtcSessionTest::aNewAccountMayForceAReadImmediately()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setServerReadCooldownMsForTest(60000);

    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    // The cooldown is per account; it does not carry over to the next one.
    client.logout();
    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 2);
    QCOMPARE(client.sessionReadPreferredServer.last(), true);
}

void RtcSessionTest::aForcedReadThatChangesNothingBacksOff()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    // 1 ms base, so the doubling is what is measured: 1, 2, 4, 8...
    controller.setServerReadCooldownMsForTest(1);

    // A participant no membership will ever account for does not cost a
    // /state request every tick for the whole call.
    for (int i = 0; i < 12; ++i) {
        controller.refreshFromServer(kRoom);
        if (client.lastSessionOp != 0) {
            Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                             sessionFor(kRoom, {}));
        }
        QTest::qWait(4);
    }
    // Without backoff every iteration would dispatch (4 ms wait > 1 ms base).
    QVERIFY2(client.sessionReads.count() < 12,
             qPrintable(QStringLiteral("every forced read dispatched (%1)")
                            .arg(client.sessionReads.count())));
    QVERIFY(client.sessionReads.count() >= 1);
}

void RtcSessionTest::aForcedReadThatFoundSomebodyRestoresFullSpeed()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    // 50 ms base: the grown gap (200 ms after two fruitless reads) is longer
    // than the 80 ms wait, and the reset gap (50 ms) shorter.
    controller.setServerReadCooldownMsForTest(50);

    // Two fruitless forced reads, each past its cooldown: the gap doubled
    // twice.
    controller.refreshFromServer(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    QTest::qWait(120);
    controller.refreshFromServer(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    QCOMPARE(client.sessionReads.count(), 2);

    // A read that finds the peer resets the backoff.
    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@b:example.org"),
                                  QStringLiteral("D2"))}));
    QCOMPARE(client.sessionReads.count(), 3);

    QTest::qWait(80);
    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 4);
}

QTEST_MAIN(RtcSessionTest)
#include "RtcSessionTest.moc"
