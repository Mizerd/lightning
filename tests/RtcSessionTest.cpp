// MatrixRTC observation (2026-08-23): RtcController's session store,
// poke coalescing, generation isolation and join-block honesty.
//
// What this suite is really defending:
//
//  * A room's participant count is what the UI counts on. It must come from
//    a reply we ASKED for, for the room we asked about, under the account we
//    asked under — a late reply from a previous account writing into the
//    current one is the generation-isolation defect §9 exists to prevent.
//  * A poke burst (a group call filling up rewrites many state events in a
//    row) must collapse into ONE read, and must not LOSE a change that
//    arrived while a read was in flight.
//  * "Cannot join" has several distinct causes and the UI wording differs
//    for each, so `joinBlockReason` must keep "not looked yet", "looked and
//    the server has nothing" and "the look failed" apart.
//  * The same person on two devices is two participants but ONE face.
#include "calls/RtcController.h"
#include "matrix/MatrixClient.h"

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
    quint64 rtcSession(const QString &roomId) override
    {
        if (!rtcSupported || refuseOps)
            return 0;
        sessionReads.append(roomId);
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

    // An op id nobody dispatched. Accepting it would let any stray poll
    // event install a participant list.
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

    // Right op id, wrong room. Writing this would attribute one room's call
    // to another.
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

    // Account switch. The in-flight read belongs to the account that is
    // going away.
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

    // A participant list is other people's presence in a room this account
    // may no longer be in; it must not survive sign-out.
    client.logout();
    QCOMPARE(controller.participantCount(kRoom), 0);
}

void RtcSessionTest::pokeBurstCollapsesIntoOneRead()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(20);

    // A group call filling up rewrites one state event per joiner. Reading
    // once per event would be pure waste.
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

    // A change arrives while that read is outstanding. The reply will carry
    // state from BEFORE this change, so dropping the poke would leave the
    // banner permanently stale.
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

    // Re-reading identical state must not re-render every banner and
    // facepile in the app.
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

    // An explicitly closed slot means the call is over even though stale
    // memberships linger, so nothing about it may be presented as live.
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

    // The local ACCOUNT is in the call, but from a different device. "Am I
    // in this call?" must not answer yes for this device.
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

    // Three participant DEVICES...
    QCOMPARE(controller.participantCount(kRoom), 3);
    // ...but two people, and a facepile showing the same avatar twice with
    // no explanation reads as a bug.
    QCOMPARE(controller.participantUserIds(kRoom).count(), 2);
    QCOMPARE(controller.participantFaces(kRoom).count(), 2);
}

void RtcSessionTest::joinBlockKeepsItsCausesApart()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    // This case is about TRANSPORT causes; say the room is unencrypted so
    // the (correct) encryption gate does not mask them.
    controller.setRoomEncrypted(kRoom, false);

    // Nothing looked yet: "checking", not "unavailable".
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("undiscovered"));

    // The server answered and named nothing: this homeserver has no
    // MatrixRTC. Different wording from a failed check.
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(client.lastTransportsOp, true,
                                        QStringLiteral("unsupported"), {},
                                        QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_transport"));
    QVERIFY(!controller.callingAvailable());

    // The check itself failed: we do not know, and must not claim the
    // server lacks calling.
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(client.lastTransportsOp, false,
                                        QStringLiteral("network"), {},
                                        QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("discovery_failed"));

    // A transport exists. Everything Matrix-side is fine, so the remaining
    // blocker is this build's missing media transport — and that is a
    // different sentence to the user than "your server has no calling".
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    QVERIFY(controller.callingAvailable());
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

    // No reads, no discovery, and an honest reason — the mock and HTTP
    // backends must not appear to support calling.
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

    // Availability is a POSITIVE fact. Before any answer it is false, and a
    // participant-advertised focus alone is enough to make it true (that is
    // how a server without the MSC4143 endpoint still supports calling).
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
    // The Rust event queue DROPS the oldest event on overflow, so a reply
    // can legitimately never arrive. Before the fix that left the room in
    // m_roomsBeingRead forever — permanently un-refreshable — while
    // flushPokes re-armed its 250 ms timer every tick for the rest of the
    // session.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setReadTimeoutMsForTest(30);

    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    // No reply ever arrives. A second read is correctly suppressed...
    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    // ...until the read times out, after which the room is retryable again.
    QTest::qWait(60);
    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 2);
}

void RtcSessionTest::aReplacedClientsLateReplyCannotBeDelivered()
{
    // Op ids come from a PER-CLIENT counter, so a replacement client
    // restarts at 1 and its ids collide with the outgoing one's. What keeps
    // the accounts apart is that setClient DISCONNECTS the previous client,
    // so its late reply is never delivered at all — deliberately not a
    // version counter, which could not discriminate a colliding id anyway
    // (the reply carries no account identity). See RtcController.h.
    FakeClient first;
    FakeClient second;
    RtcController controller;
    controller.setClient(&first);
    controller.refresh(kRoom);
    const quint64 staleOp = first.lastSessionOp;

    controller.setClient(&second);
    // The new client hands out the SAME op id for its own first read.
    controller.refresh(kRoom);
    QCOMPARE(second.lastSessionOp, staleOp);

    // The OLD client's reply, carrying that id, must be refused.
    Q_EMIT first.rtcSessionReceived(
        staleOp, sessionFor(kRoom, {person(QStringLiteral("@ghost:example.org"),
                                           QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 0);

    // The NEW client's reply for the same id is accepted.
    Q_EMIT second.rtcSessionReceived(
        second.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@real:example.org"),
                                  QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 1);
}

void RtcSessionTest::oneRoomsFocusDoesNotDecideAnothers()
{
    // A focus advertised by ROOM A's participants says nothing about room
    // B. Consulting it account-globally let one room's SFU decide another
    // room's join-block wording.
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    // About focus SCOPING; both rooms unencrypted so the encryption gate
    // does not mask the transport answer.
    controller.setRoomEncrypted(kRoom, false);
    controller.setRoomEncrypted(kOther, false);

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(), {},
        QStringLiteral("https://room-a-focus.example.org/"));

    // Room A has a reachable transport...
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    // ...room B does not, and must say so.
    QCOMPARE(controller.joinBlockReason(kOther),
             QStringLiteral("no_transport"));
}

void RtcSessionTest::anEncryptedRoomRefusesWithoutMediaEncryption()
{
    // §6: a call in an end-to-end encrypted room must FAIL SAFELY rather
    // than publish media the SFU can read. The user was told that room is
    // encrypted; quietly carrying their audio in the clear because the
    // frame cryptor is not wired yet would make that a lie.
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

    // Encrypted room, media E2EE not active: a DIFFERENT and more important
    // refusal, and it must be the one reported.
    controller.setRoomEncrypted(kRoom, true);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("media_encryption_unavailable"));

    // With media E2EE active the encryption objection lifts, leaving only
    // the ordinary transport blocker.
    controller.setMediaEncryptionAvailable(true);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));

    // A room we were never told about defaults to ENCRYPTED. A boolean
    // cannot say "unknown", and the safe answer to "might this be
    // encrypted?" is yes — assuming unencrypted would be exactly the
    // silent downgrade the gate exists to prevent.
    RtcController fresh;
    FakeClient freshClient;
    fresh.setClient(&freshClient);
    fresh.discover(kOther);
    Q_EMIT freshClient.rtcTransportsReceived(
        freshClient.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    QCOMPARE(fresh.joinBlockReason(kOther),
             QStringLiteral("media_encryption_unavailable"));
}

QTEST_MAIN(RtcSessionTest)
#include "RtcSessionTest.moc"
