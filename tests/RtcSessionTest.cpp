#include "calls/RtcController.h"
#include "matrix/MatrixClient.h"

#include <QDateTime>
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
    bool routed = false;
    bool networkRouteActive() const override { return routed; }
    QStringList sessionReads;
    QList<bool> sessionReadPreferredServer;
    QStringList transportRooms;
    QStringList memberRequests;
    quint64 lastSessionOp = 0;
    quint64 lastTransportsOp = 0;
    quint64 lastMembersOp = 0;

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
    quint64 requestRoomMembers(const QString &roomId) override
    {
        if (refuseOps)
            return 0;
        memberRequests.append(roomId);
        lastMembersOp = nextOp++;
        return lastMembersOp;
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
    p.expiresAtMs = QDateTime::currentMSecsSinceEpoch() + 3600000;
    p.wireFormat = QStringLiteral("session");
    p.ownUser = ownUser;
    p.ownDevice = ownDevice;
    return p;
}

QVariantMap memberSnapshot(bool canPublish, bool partial = false)
{
    return QVariantMap{
        {QStringLiteral("ok"), true},
        {QStringLiteral("partial"), partial},
        {QStringLiteral("canPublishCallMembership"), canPublish},
    };
}

RtcSessionData sessionFor(const QString &roomId,
                          const QVector<RtcParticipant> &people)
{
    RtcSessionData data;
    data.roomId = roomId;
    data.participants = people;
    return data;
}

}

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
    void aRoutedAccountIsBlockedFromJoiningWhateverElseIsTrue();
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
    void anExpiredMembershipLeavesTheSessionWithoutAPoke();
    void anUnrelatedReadInsideTheGraceWindowKeepsTheExpiryReread();
    void aReadDispatchedBeforeTheExpiryDoesNotCoverIt();
    void aMembershipTheStoreKeepsIsNotReReadForever();
    void anOrdinaryRefreshTrustsTheLocalStore();
    void aForcedRefreshAsksTheHomeserver();
    void aForcedRefreshIsRateLimitedPerRoom();
    void aForcedRefreshDuringAnInFlightReadIsNotLost();
    void aNewAccountMayForceAReadImmediately();
    void aForcedReadThatChangesNothingBacksOff();
    void aForcedReadThatFoundSomebodyRestoresFullSpeed();
    void aMemberSnapshotNobodyPushedStillGatesTheLane();
    void openingARoomReadsItsPermissionOnce();
    void aForbiddenRefusalBlocksTheLane();
    void aPokeClearsTheRefusalAndReReadsTheOpenRoom();
    void anUnansweredPermissionReadDoesNotBlockTheReRead();
    void openingAnotherRoomDropsTheRefusal();
    void signOutForgetsPermissionAndRefusals();
    void onePermissionReadPerRoomAtATime();

private:
    static void makeJoinable(RtcController &controller, FakeClient &client);
};

void RtcSessionTest::makeJoinable(RtcController &controller,
                                  FakeClient &client)
{
    controller.setMediaAvailable(true);
    controller.setRoomEncrypted(kRoom, false);
    controller.setRoomEncrypted(kOther, false);
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
}

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

    client.logout();
    QCOMPARE(controller.participantCount(kRoom), 0);
}

void RtcSessionTest::pokeBurstCollapsesIntoOneRead()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(20);

    for (int i = 0; i < 8; ++i)
        Q_EMIT client.rtcSessionChanged(kRoom);
    QCOMPARE(client.sessionReads.count(), 0);

    QTRY_COMPARE(client.sessionReads.count(), 1);
    QCOMPARE(client.sessionReads.first(), kRoom);
}

void RtcSessionTest::aPokeDuringAnInFlightReadIsNotLost()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(10);

    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    Q_EMIT client.rtcSessionChanged(kRoom);
    QTest::qWait(40);
    QCOMPARE(client.sessionReads.count(), 1);

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

    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@me:example.org"),
                                  QStringLiteral("OTHER"), 1000,
true,false)}));
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

    QCOMPARE(controller.participantCount(kRoom), 3);
    QCOMPARE(controller.participantUserIds(kRoom).count(), 2);
    QCOMPARE(controller.participantFaces(kRoom).count(), 2);
}

void RtcSessionTest::aRoutedAccountIsBlockedFromJoiningWhateverElseIsTrue()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setRoomEncrypted(kRoom, false);
    controller.setMediaAvailable(true);
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);

    client.routed = true;
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::NetworkRoute);
    QCOMPARE(controller.joinBlockReason(kRoom), QStringLiteral("network_route"));
    client.routed = false;
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
}

void RtcSessionTest::joinBlockKeepsItsCausesApart()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setRoomEncrypted(kRoom, false);

    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("undiscovered"));
    QVERIFY(controller.discoveryWorthRetrying());

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(client.lastTransportsOp, true,
                                        QStringLiteral("unsupported"), {},
                                        QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_transport"));
    QVERIFY(!controller.callingAvailable());
    QVERIFY2(!controller.discoveryWorthRetrying(),
             "a homeserver that answered \"no MatrixRTC here\" is still "
             "re-asked on every room change");

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(client.lastTransportsOp, false,
                                        QStringLiteral("network"), {},
                                        QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("discovery_failed"));
    QVERIFY(controller.discoveryWorthRetrying());

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    QVERIFY(controller.callingAvailable());

    controller.setMediaAvailable(true);
    QVERIFY(controller.joinBlockReason(kRoom).isEmpty());
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);

    controller.setCanPublishMembership(kRoom, false);
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::NoPermission);
    QCOMPARE(controller.joinBlockReason(kRoom), QStringLiteral("no_permission"));

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

    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

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
    controller.refresh(kRoom);
    QCOMPARE(second.lastSessionOp, staleOp);

    Q_EMIT first.rtcSessionReceived(
        staleOp, sessionFor(kRoom, {person(QStringLiteral("@ghost:example.org"),
                                           QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 0);

    Q_EMIT second.rtcSessionReceived(
        second.lastSessionOp,
        sessionFor(kRoom, {person(QStringLiteral("@real:example.org"),
                                  QStringLiteral("D1"))}));
    QCOMPARE(controller.participantCount(kRoom), 1);
}

void RtcSessionTest::oneRoomsFocusDoesNotDecideAnothers()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setRoomEncrypted(kRoom, false);
    controller.setRoomEncrypted(kOther, false);

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(), {},
        QStringLiteral("https://room-a-focus.example.org/"));

    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    QCOMPARE(controller.joinBlockReason(kOther),
             QStringLiteral("no_transport"));
}

void RtcSessionTest::aSessionThatNamesAFocusIsJoinableWithoutDiscovery()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setRoomEncrypted(kRoom, false);
    controller.setMediaAvailable(true);
    controller.setMediaEncryptionAvailable(true);

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
    QVERIFY(controller.callingAvailable());

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
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setRoomEncrypted(kRoom, false);

    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://ours.example.org/")}, QString());
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

    RtcSessionData closed = session;
    closed.slotClosed = true;
    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp, closed);
    QCOMPARE(controller.focusUrlFor(kRoom),
             QStringLiteral("https://ours.example.org/"));
}

void RtcSessionTest::mediaKeyTargetsAddressEveryOtherDeviceAndNotOurOwn()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(
        client.lastSessionOp,
        sessionFor(kRoom,
                   {person(QStringLiteral("@me:example.org"),
                           QStringLiteral("MINE"), 1000,
true,true),
                    person(QStringLiteral("@me:example.org"),
                           QStringLiteral("OTHER"), 1100,
true,false),
                    person(QStringLiteral("@them:example.org"),
                           QStringLiteral("THEIRS"), 1200,
false,false)}));

    const QJsonArray targets =
        QJsonDocument::fromJson(
            controller.mediaKeyTargetsJson(kRoom).toUtf8()).array();
    QCOMPARE(targets.size(), 2);

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
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.discover(kRoom);
    Q_EMIT client.rtcTransportsReceived(
        client.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());

    controller.setRoomEncrypted(kRoom, false);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));

    controller.setRoomEncrypted(kRoom, true);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("media_encryption_unavailable"));

    controller.setMediaEncryptionAvailable(true);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_media_transport"));
    controller.setMediaAvailable(true);
    QVERIFY(controller.joinBlockReason(kRoom).isEmpty());

    RtcController fresh;
    FakeClient freshClient;
    fresh.setClient(&freshClient);
    fresh.discover(kOther);
    Q_EMIT freshClient.rtcTransportsReceived(
        freshClient.lastTransportsOp, true, QString(),
        QStringList{QStringLiteral("https://sfu.example.org/")}, QString());
    QCOMPARE(fresh.joinBlockReason(kOther),
             QStringLiteral("media_encryption_unavailable"));
    fresh.setMediaAvailable(true);
    QCOMPARE(fresh.joinBlockReason(kOther),
             QStringLiteral("media_encryption_unavailable"));
}

// Server-backed reads. The SFU only lists participants who authenticated as
// real Matrix identities, so one that no membership accounts for means our
// local state is incomplete; a forced /state read is the way back.

void RtcSessionTest::anExpiredMembershipLeavesTheSessionWithoutAPoke()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(10);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    RtcParticipant live = person(QStringLiteral("@a:example.org"),
                                 QStringLiteral("D1"), now);
    live.expiresAtMs = now + 3600000;
    RtcParticipant ghost = person(QStringLiteral("@ghost:example.org"),
                                  QStringLiteral("GHOST"), now - 290000);
    ghost.expiresAtMs = now + 200;

    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {live, ghost}));
    QCOMPARE(controller.participantCount(kRoom), 2);
    QCOMPARE(client.sessionReads.count(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(client.sessionReads.count(), 2, 3000);
    QCOMPARE(client.sessionReads.last(), kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {live}));
    QCOMPARE(controller.participantCount(kRoom), 1);

    QTest::qWait(1500);
    QCOMPARE(client.sessionReads.count(), 2);
}

void RtcSessionTest::anUnrelatedReadInsideTheGraceWindowKeepsTheExpiryReread()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(10);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    RtcParticipant live = person(QStringLiteral("@a:example.org"),
                                 QStringLiteral("D1"), now);
    live.expiresAtMs = now + 3600000;
    RtcParticipant ghost = person(QStringLiteral("@ghost:example.org"),
                                  QStringLiteral("GHOST"), now - 290000);
    ghost.expiresAtMs = now + 200;

    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {live, ghost}));
    controller.refresh(kOther);
    const quint64 otherOp = client.lastSessionOp;

    QTest::qWait(500);
    Q_EMIT client.rtcSessionReceived(otherOp, sessionFor(kOther, {live}));

    QTRY_COMPARE_WITH_TIMEOUT(client.sessionReads.count(kRoom), 2, 3000);
}

void RtcSessionTest::aReadDispatchedBeforeTheExpiryDoesNotCoverIt()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(10);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    RtcParticipant live = person(QStringLiteral("@a:example.org"),
                                 QStringLiteral("D1"), now);
    live.expiresAtMs = now + 3600000;
    RtcParticipant ghost = person(QStringLiteral("@ghost:example.org"),
                                  QStringLiteral("GHOST"), now - 290000);
    ghost.expiresAtMs = now + 300;

    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {live, ghost}));
    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(kRoom), 2);
    QTest::qWait(500);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {live, ghost}));
    QCOMPARE(controller.participantCount(kRoom), 2);

    QTRY_COMPARE_WITH_TIMEOUT(client.sessionReads.count(kRoom), 3, 3000);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {live}));
    QCOMPARE(controller.participantCount(kRoom), 1);

    QTest::qWait(1500);
    QCOMPARE(client.sessionReads.count(kRoom), 3);
}

void RtcSessionTest::aMembershipTheStoreKeepsIsNotReReadForever()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPokeCoalesceMsForTest(10);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    RtcParticipant ghost = person(QStringLiteral("@ghost:example.org"),
                                  QStringLiteral("GHOST"), now - 290000);
    ghost.expiresAtMs = now + 150;

    controller.refresh(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {ghost}));
    QTRY_COMPARE_WITH_TIMEOUT(client.sessionReads.count(kRoom), 2, 3000);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {ghost}));
    QTest::qWait(1500);
    QCOMPARE(client.sessionReads.count(kRoom), 2);
}

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

    controller.refreshFromServer(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    controller.refreshFromServer(kRoom);
    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

    controller.refreshFromServer(QStringLiteral("!other:example.org"));
    QCOMPARE(client.sessionReads.count(), 2);
    QCOMPARE(client.sessionReadPreferredServer.last(), true);

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

    controller.refresh(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);
    QCOMPARE(client.sessionReadPreferredServer.first(), false);

    controller.refreshFromServer(kRoom);
    QCOMPARE(client.sessionReads.count(), 1);

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
    controller.setServerReadCooldownMsForTest(1);

    for (int i = 0; i < 12; ++i) {
        controller.refreshFromServer(kRoom);
        if (client.lastSessionOp != 0) {
            Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                             sessionFor(kRoom, {}));
        }
        QTest::qWait(4);
    }
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
    controller.setServerReadCooldownMsForTest(50);

    controller.refreshFromServer(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    QTest::qWait(120);
    controller.refreshFromServer(kRoom);
    Q_EMIT client.rtcSessionReceived(client.lastSessionOp,
                                     sessionFor(kRoom, {}));
    QCOMPARE(client.sessionReads.count(), 2);

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

void RtcSessionTest::aMemberSnapshotNobodyPushedStillGatesTheLane()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    makeJoinable(controller, client);
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
    QSignalSpy changed(&controller, &RtcController::sessionChanged);

    Q_EMIT client.roomMembersReceived(
        77, kRoom, QVariantMap{{QStringLiteral("ok"), false}});
    Q_EMIT client.roomMembersReceived(
        78, kRoom, QVariantMap{{QStringLiteral("ok"), true}});
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);

    Q_EMIT client.roomMembersReceived(79, kRoom, memberSnapshot(false));
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);
    QVERIFY(!changed.isEmpty());
    QCOMPARE(controller.joinBlock(kOther), RtcController::JoinBlock::None);

    Q_EMIT client.roomMembersReceived(80, kRoom, memberSnapshot(true));
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
}

void RtcSessionTest::openingARoomReadsItsPermissionOnce()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);

    controller.setCurrentRoom(kRoom, true);
    QVERIFY(client.memberRequests.isEmpty());

    controller.setCurrentRoom(kOther);
    QCOMPARE(client.memberRequests, QStringList{kOther});
    Q_EMIT client.roomMembersReceived(client.lastMembersOp, kOther,
                                      memberSnapshot(false));

    controller.setCurrentRoom(kRoom);
    QCOMPARE(client.memberRequests, (QStringList{kOther, kRoom}));
    Q_EMIT client.roomMembersReceived(client.lastMembersOp, kRoom,
                                      memberSnapshot(true));
    controller.setCurrentRoom(kOther);
    controller.setCurrentRoom(kRoom);
    QCOMPARE(client.memberRequests.count(), 2);

    Q_EMIT client.roomMemberEventSeen(kOther);
    controller.setCurrentRoom(kOther);
    QCOMPARE(client.memberRequests.count(), 3);
    QCOMPARE(client.memberRequests.last(), kOther);

    FakeClient plain;
    plain.rtcSupported = false;
    RtcController other;
    other.setClient(&plain);
    other.setCurrentRoom(kRoom);
    QVERIFY(plain.memberRequests.isEmpty());
}

void RtcSessionTest::aForbiddenRefusalBlocksTheLane()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    makeJoinable(controller, client);
    controller.setCurrentRoom(kRoom, true);
    Q_EMIT client.roomMembersReceived(90, kRoom, memberSnapshot(true));
    QSignalSpy refused(&controller, &RtcController::membershipRefused);
    QSignalSpy changed(&controller, &RtcController::sessionChanged);

    controller.noteMembershipRefused(kRoom, QStringLiteral("network"));
    controller.noteMembershipRefused(kRoom, QStringLiteral("rate_limited"));
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
    QCOMPARE(refused.count(), 0);

    controller.noteMembershipRefused(kRoom, QStringLiteral("forbidden"));
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);
    QCOMPARE(controller.joinBlockReason(kRoom),
             QStringLiteral("no_permission"));
    QCOMPARE(refused.count(), 1);
    QCOMPARE(refused.at(0).at(0).toString(), kRoom);
    QVERIFY(!changed.isEmpty());
    Q_EMIT client.roomMembersReceived(91, kRoom, memberSnapshot(true));
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);
    QCOMPARE(controller.joinBlock(kOther), RtcController::JoinBlock::None);
}

void RtcSessionTest::aPokeClearsTheRefusalAndReReadsTheOpenRoom()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPermissionRereadMsForTest(0);
    makeJoinable(controller, client);
    controller.setCurrentRoom(kRoom, true);
    Q_EMIT client.roomMembersReceived(95, kRoom, memberSnapshot(false));
    controller.noteMembershipRefused(kRoom, QStringLiteral("forbidden"));
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);

    Q_EMIT client.roomMemberEventSeen(kRoom);
    QTRY_COMPARE(client.memberRequests, QStringList{kRoom});
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);
    Q_EMIT client.roomMembersReceived(client.lastMembersOp, kRoom,
                                      memberSnapshot(true));
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);

    Q_EMIT client.roomMemberEventSeen(kRoom);
    Q_EMIT client.roomMembersReceived(96, kOther, memberSnapshot(false));
    Q_EMIT client.roomMemberEventSeen(kOther);
    QTest::qWait(20);
    QCOMPARE(client.memberRequests.count(), 1);

    controller.noteMembershipRefused(kRoom, QStringLiteral("forbidden"));
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);
    Q_EMIT client.roomMemberEventSeen(kRoom);
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
}

void RtcSessionTest::anUnansweredPermissionReadDoesNotBlockTheReRead()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setPermissionRereadMsForTest(5);
    controller.setReadTimeoutMsForTest(30);
    makeJoinable(controller, client);
    controller.setCurrentRoom(kRoom, true);
    Q_EMIT client.roomMembersReceived(95, kRoom, memberSnapshot(false));

    Q_EMIT client.roomMemberEventSeen(kRoom);
    QTRY_COMPARE(client.memberRequests.count(), 1);
    Q_EMIT client.roomMemberEventSeen(kRoom);
    QTRY_VERIFY(client.memberRequests.count() >= 2);
    Q_EMIT client.roomMembersReceived(client.lastMembersOp, kRoom,
                                      memberSnapshot(true));
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
}

void RtcSessionTest::openingAnotherRoomDropsTheRefusal()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    makeJoinable(controller, client);
    controller.setCurrentRoom(kRoom, true);
    controller.noteMembershipRefused(kRoom, QStringLiteral("forbidden"));
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);

    controller.setCurrentRoom(kRoom, true);
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);

    QSignalSpy changed(&controller, &RtcController::sessionChanged);
    controller.setCurrentRoom(kOther, true);
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
    QVERIFY(changed.contains(QVariantList{kRoom}));
}

void RtcSessionTest::signOutForgetsPermissionAndRefusals()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    makeJoinable(controller, client);
    controller.setCurrentRoom(kRoom, true);
    Q_EMIT client.roomMembersReceived(97, kOther, memberSnapshot(false));
    controller.noteMembershipRefused(kRoom, QStringLiteral("forbidden"));

    client.logout();
    makeJoinable(controller, client);
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
    QCOMPARE(controller.joinBlock(kOther), RtcController::JoinBlock::None);
    controller.setCurrentRoom(kRoom);
    QCOMPARE(client.memberRequests, QStringList{kRoom});
}

void RtcSessionTest::onePermissionReadPerRoomAtATime()
{
    FakeClient client;
    RtcController controller;
    controller.setClient(&client);
    controller.setCurrentRoom(kRoom);
    QCOMPARE(client.memberRequests.count(), 1);
    const quint64 op = client.lastMembersOp;

    Q_EMIT client.roomMembersReceived(op, kRoom, memberSnapshot(false, true));
    makeJoinable(controller, client);
    QCOMPARE(controller.joinBlock(kRoom),
             RtcController::JoinBlock::NoPermission);
    Q_EMIT client.roomMemberEventSeen(kOther);
    Q_EMIT client.roomMemberEventSeen(kRoom);
    controller.setCurrentRoom(kRoom);
    QCOMPARE(client.memberRequests.count(), 1);

    Q_EMIT client.roomMembersReceived(op, kRoom, memberSnapshot(true));
    QCOMPARE(controller.joinBlock(kRoom), RtcController::JoinBlock::None);
    controller.setCurrentRoom(kRoom);
    QCOMPARE(client.memberRequests.count(), 1);
    Q_EMIT client.roomMemberEventSeen(kRoom);
    controller.setCurrentRoom(kRoom);
    QCOMPARE(client.memberRequests.count(), 2);
}

QTEST_MAIN(RtcSessionTest)
#include "RtcSessionTest.moc"
