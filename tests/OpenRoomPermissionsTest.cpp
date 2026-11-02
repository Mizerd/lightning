
#include "app/OpenRoomPermissions.h"
#include "matrix/MatrixClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    bool refuse = false;
    QStringList asked;
    quint64 lastOpId = 0;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> roomList;
    QList<RoomInfo> rooms() const override { return roomList; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
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

    quint64 requestRoomOwnPermissions(const QString &roomId) override
    {
        asked.append(roomId);
        if (refuse)
            return 0;
        lastOpId = nextOp++;
        return lastOpId;
    }

    void answer(quint64 op, const QString &roomId, const QVariantMap &facts)
    {
        Q_EMIT roomOwnPermissionsReceived(op, roomId, facts);
    }
    void poke(const QString &roomId) { Q_EMIT roomMemberEventSeen(roomId); }
    void setRoom(const QString &id, bool encrypted, bool known = true)
    {
        for (RoomInfo &r : roomList) {
            if (r.id == id) {
                r.encrypted = encrypted;
                r.encryptionKnown = known;
                Q_EMIT roomsChanged();
                return;
            }
        }
        RoomInfo r;
        r.id = id;
        r.encrypted = encrypted;
        r.encryptionKnown = known;
        roomList.append(r);
        Q_EMIT roomsChanged();
    }
};

const QString kRoom = QStringLiteral("!a:example.org");
const QString kOther = QStringLiteral("!b:example.org");

QVariantMap facts(const QString &membership, const QVariant &send,
                  const QVariant &redact)
{
    QVariantMap out;
    out.insert(QStringLiteral("ownMembership"), membership);
    if (send.isValid())
        out.insert(QStringLiteral("canSendMessages"), send);
    if (redact.isValid())
        out.insert(QStringLiteral("canRedactOthers"), redact);
    return out;
}

}

class OpenRoomPermissionsTest : public QObject
{
    Q_OBJECT

private slots:
    void unknownIsAllowedToPostAndNotOfferedToModerate()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        QCOMPARE(client.asked, QStringList{kRoom});
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.sendKnown());
        QVERIFY(perms.sendBlockedReason().isEmpty());
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(!perms.redactKnown());

        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), {}, {}));
        QCOMPARE(perms.ownMembership(), QStringLiteral("joined"));
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.sendKnown());
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(!perms.redactKnown());
    }

    void anAnswerForTheOpenRoomIsApplied()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        QSignalSpy changed(&perms, &OpenRoomPermissions::changed);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QCOMPARE(changed.count(), 1);
        QVERIFY(perms.sendKnown());
        QVERIFY(!perms.canSendMessages());
        QCOMPARE(perms.sendBlockedReason(),
                 QStringLiteral("You do not have permission to post in this "
                                "room."));
        QVERIFY(perms.redactKnown());
        QVERIFY(perms.canRedactOthers());
    }

    void theCommandFactsAreMappedAndOnlyKnownKeysAppear()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        client.poke(kRoom);
        QVERIFY(perms.commandFacts().isEmpty());

        QVariantMap answer = facts(QStringLiteral("joined"), true, false);
        answer.insert(QStringLiteral("canKick"), true);
        answer.insert(QStringLiteral("canBan"), false);
        answer.insert(QStringLiteral("canUnban"), true);
        answer.insert(QStringLiteral("canInvite"), false);
        answer.insert(QStringLiteral("canEditTopic"), true);
        answer.insert(QStringLiteral("canEditName"), false);
        client.answer(client.lastOpId, kRoom, answer);
        QVariantMap expected;
        expected.insert(QStringLiteral("kick"), true);
        expected.insert(QStringLiteral("ban"), false);
        expected.insert(QStringLiteral("unban"), true);
        expected.insert(QStringLiteral("invite"), false);
        expected.insert(QStringLiteral("topic"), true);
        expected.insert(QStringLiteral("roomname"), false);
        QCOMPARE(perms.commandFacts(), expected);

        QSignalSpy changed(&perms, &OpenRoomPermissions::changed);
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom, answer);
        QCOMPARE(changed.count(), 0);
        answer.insert(QStringLiteral("canBan"), true);
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom, answer);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(perms.commandFacts().value(QStringLiteral("ban")).toBool(),
                 true);

        QVariantMap partial = facts(QStringLiteral("joined"), true, false);
        partial.insert(QStringLiteral("canInvite"), true);
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom, partial);
        QCOMPARE(perms.commandFacts().size(), 1);
        QVERIFY(perms.commandFacts().value(QStringLiteral("invite")).toBool());

        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("left"), {}, {}));
        const QVariantMap off = perms.commandFacts();
        QCOMPARE(off.size(), 6);
        for (auto it = off.cbegin(); it != off.cend(); ++it)
            QVERIFY2(!it.value().toBool(), qPrintable(it.key()));
    }

    void aRoomWeAreNotInBlocksPostingWithItsReason()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        for (const QString membership :
             {QStringLiteral("left"), QStringLiteral("invited"),
              QStringLiteral("banned")}) {
            perms.setRoomId(QString());
            perms.setRoomId(kRoom);
            client.answer(client.lastOpId, kRoom, facts(membership, {}, {}));
            QVERIFY2(perms.sendKnown(), qPrintable(membership));
            QVERIFY2(!perms.canSendMessages(), qPrintable(membership));
            QCOMPARE(perms.sendBlockedReason(),
                     QStringLiteral("You are not in this room."));
            QVERIFY(!perms.canRedactOthers());
        }
    }

    void anotherRoomsOrAnOlderAnswerIsNeverShown()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        const quint64 firstOp = client.lastOpId;
        client.answer(firstOp, kRoom, facts(QStringLiteral("joined"), false, true));
        QVERIFY(!perms.canSendMessages());

        perms.setRoomId(kOther);
        QCOMPARE(perms.roomId(), kOther);
        QVERIFY2(perms.canSendMessages() && !perms.sendKnown(),
                 "the previous room's refusal survived the switch");
        QVERIFY2(!perms.canRedactOthers() && !perms.redactKnown(),
                 "the previous room's grant survived the switch");
        client.answer(firstOp, kRoom, facts(QStringLiteral("joined"), false, true));
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.canRedactOthers());

        const quint64 superseded = client.lastOpId;
        client.poke(kOther);
        QVERIFY(client.lastOpId != superseded);
        client.answer(superseded, kOther, facts(QStringLiteral("joined"), false, true));
        QVERIFY(perms.canSendMessages());
        client.answer(client.lastOpId, kOther,
                      facts(QStringLiteral("joined"), true, false));
        QVERIFY(perms.sendKnown());
        QVERIFY(perms.canSendMessages());
        QVERIFY(perms.redactKnown());
        QVERIFY(!perms.canRedactOthers());
    }

    void aPokeForTheOpenRoomReadsItAgain()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), true, true));
        client.asked.clear();

        client.poke(kOther);
        QVERIFY2(client.asked.isEmpty(), "another room's poke was read");
        client.poke(kRoom);
        QCOMPARE(client.asked, QStringList{kRoom});
        QVERIFY2(perms.canRedactOthers(),
                 "the answer was dropped before the re-read landed");
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), true, false));
        QVERIFY(!perms.canRedactOthers());
    }

    void anIdenticalAnswerDoesNotNotify()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QSignalSpy changed(&perms, &OpenRoomPermissions::changed);
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QCOMPARE(changed.count(), 0);
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, false));
        QCOMPARE(changed.count(), 1);
        QVERIFY(!perms.canRedactOthers());
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, {}));
        QCOMPARE(changed.count(), 2);
        QVERIFY(!perms.redactKnown());
    }

    void aRefusedRequestStaysUnknown()
    {
        FakeClient client;
        client.refuse = true;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        client.answer(0, kRoom, facts(QStringLiteral("left"), false, true));
        client.answer(7, kRoom, facts(QStringLiteral("left"), false, true));
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.sendKnown());
        QVERIFY(!perms.canRedactOthers());
    }

    void signOutClearsTheFacts()
    {
        FakeClient client;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        const quint64 op = client.lastOpId;
        client.answer(op, kRoom, facts(QStringLiteral("joined"), false, true));
        QVERIFY(perms.canRedactOthers());
        client.logout();
        QVERIFY(perms.roomId().isEmpty());
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(perms.ownMembership().isEmpty());
        client.answer(op, kRoom, facts(QStringLiteral("joined"), false, true));
        QVERIFY(!perms.canRedactOthers());

        FakeClient next;
        perms.setRoomId(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY(perms.canRedactOthers());
        perms.setClient(&next);
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(perms.canSendMessages());
        QCOMPARE(next.asked, QStringList{kRoom});
        client.answer(next.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY2(!perms.canRedactOthers(),
                 "the previous account's client still reaches the facts");
    }

    void encryptionTurningOnReReadsTheFacts()
    {
        FakeClient client;
        client.setRoom(kRoom, false);
        client.setRoom(kOther, false);
        OpenRoomPermissions perms;
        perms.setClient(&client);
        perms.setRoomId(kRoom);
        QCOMPARE(client.asked, QStringList{kRoom});
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), true, true));
        QVERIFY(perms.canSendMessages());
        QVERIFY(perms.canRedactOthers());

        client.setRoom(kOther, true);
        client.setRoom(kRoom, false);
        QVERIFY2(client.asked == QStringList{kRoom},
                 "a room-list change that left this room alone re-read");

        client.setRoom(kRoom, true);
        QCOMPARE(client.asked, (QStringList{kRoom, kRoom}));
        client.setRoom(kRoom, true);
        QCOMPARE(client.asked.size(), 2);
        QVERIFY(perms.canRedactOthers());
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY(!perms.canSendMessages());
        QVERIFY(perms.sendKnown());
    }

    void encryptionKnownArrivingAndAnUnansweredReadStayPermissive()
    {
        FakeClient client;
        client.refuse = true;
        OpenRoomPermissions perms;
        perms.setClient(&client);
        client.setRoom(kRoom, false, false);
        perms.setRoomId(kRoom);
        QCOMPARE(client.asked.size(), 1);
        client.setRoom(kRoom, true, true);
        QCOMPARE(client.asked.size(), 2);
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.sendKnown());
        QVERIFY(perms.sendBlockedReason().isEmpty());
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(!perms.redactKnown());
        perms.setRoomId(QString());
        client.setRoom(kRoom, false, true);
        QCOMPARE(client.asked.size(), 2);
    }
};

QTEST_GUILESS_MAIN(OpenRoomPermissionsTest)
#include "OpenRoomPermissionsTest.moc"
