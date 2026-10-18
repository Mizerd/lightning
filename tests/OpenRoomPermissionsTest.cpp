// OpenRoomPermissions: the open room's own posting facts (break-it B025,
// B027 and B123 follow-up). They follow the open room, are read on every open
// and on every member or power-level poke for it, are never shown for another
// room or from a superseded request, are cleared on sign-out, and read
// unknown exactly as RoomInfoController reads the same answers: allowed for
// posting, not offered for moderation.

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

    // MatrixClient pure virtuals (inert).
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

} // namespace

class OpenRoomPermissionsTest : public QObject
{
    Q_OBJECT

private slots:
    // Nothing answered yet: posting allowed, moderation not offered, and
    // neither counts as known, so the composer may fall back to the room-info
    // controller.
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

        // An answer with both permissions null: still unknown.
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), {}, {}));
        QCOMPARE(perms.ownMembership(), QStringLiteral("joined"));
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.sendKnown());
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(!perms.redactKnown());
    }

    // The two answers are applied separately: two DIFFERENT values, so a
    // swapped key fails.
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

    // The slash-command facts map answer keys to hint keys, only for the keys
    // the answer carried (six DIFFERENT values, so a swapped key fails), and a
    // known not-joined membership turns every command off. Unknown stays
    // absent, never false. An unchanged answer does not notify; a changed
    // moderation fact alone does.
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
        client.poke(kRoom); // ask again
        client.answer(client.lastOpId, kRoom, answer);
        QCOMPARE(changed.count(), 0);
        answer.insert(QStringLiteral("canBan"), true);
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom, answer);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(perms.commandFacts().value(QStringLiteral("ban")).toBool(),
                 true);

        // A partial answer carries only what it knows.
        QVariantMap partial = facts(QStringLiteral("joined"), true, false);
        partial.insert(QStringLiteral("canInvite"), true);
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom, partial);
        QCOMPARE(perms.commandFacts().size(), 1);
        QVERIFY(perms.commandFacts().value(QStringLiteral("invite")).toBool());

        // Not joined: every command off, whatever else was said.
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("left"), {}, {}));
        const QVariantMap off = perms.commandFacts();
        QCOMPARE(off.size(), 6);
        for (auto it = off.cbegin(); it != off.cend(); ++it)
            QVERIFY2(!it.value().toBool(), qPrintable(it.key()));
    }

    // A membership that is not "joined" blocks posting by itself, with its
    // own reason, and grants nothing.
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

    // Another room's answer, and a request a newer one superseded, are
    // dropped; switching rooms clears the facts before the next answer.
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
        // The old room's answer, under its old op or the new one.
        client.answer(firstOp, kRoom, facts(QStringLiteral("joined"), false, true));
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.canRedactOthers());

        // A poke supersedes the read in flight: its answer is dropped.
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

    // Membership and power-level changes arrive on one poke: only the open
    // room's is read, and the previous answer stays until the new one lands.
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
        // Demoted: the grant goes.
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), true, false));
        QVERIFY(!perms.canRedactOthers());
    }

    // Every timeline row binds canRedactOthers, so an answer that changes
    // nothing (a poke in a busy room) must not notify.
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
        // A real change still does, once.
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, false));
        QCOMPARE(changed.count(), 1);
        QVERIFY(!perms.canRedactOthers());
        // Unknown is not the same as an answer of the same value.
        client.poke(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, {}));
        QCOMPARE(changed.count(), 2);
        QVERIFY(!perms.redactKnown());
    }

    // A backend that cannot answer (0) leaves the facts unknown, and an
    // answer to an op nobody holds is ignored.
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

    // Sign-out ends the room: nothing of this account's answer remains, and
    // no late answer brings it back.
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

        // A different client (account switch) starts from nothing too.
        FakeClient next;
        perms.setRoomId(kRoom);
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY(perms.canRedactOthers());
        perms.setClient(&next);
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(perms.canSendMessages());
        QCOMPARE(next.asked, QStringList{kRoom});
        // The old client answers under the op the NEW client issued, which
        // the op check alone would accept: only the disconnect stops it.
        client.answer(next.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY2(!perms.canRedactOthers(),
                 "the previous account's client still reaches the facts");
    }

    // The open room turns encryption on: the facts are read again from the
    // same local state, the old answer stays until the new one lands (unknown
    // stays allowed-to-post and not-offered-to-moderate), and room-list
    // changes that say nothing new about THIS room ask nothing.
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

        // Unrelated room-list changes: another room, and this room unchanged.
        client.setRoom(kOther, true);
        client.setRoom(kRoom, false);
        QVERIFY2(client.asked == QStringList{kRoom},
                 "a room-list change that left this room alone re-read");

        // This room turns encryption on: one more read.
        client.setRoom(kRoom, true);
        QCOMPARE(client.asked, (QStringList{kRoom, kRoom}));
        // Repeats of the same state ask nothing.
        client.setRoom(kRoom, true);
        QCOMPARE(client.asked.size(), 2);
        // Until it answers the previous answer stands.
        QVERIFY(perms.canRedactOthers());
        // The new answer is applied: send now refused.
        client.answer(client.lastOpId, kRoom,
                      facts(QStringLiteral("joined"), false, true));
        QVERIFY(!perms.canSendMessages());
        QVERIFY(perms.sendKnown());
    }

    // The state only becomes known after the open (encryptionKnown flips):
    // that is a change too. And a room that is not yet in the list, then
    // appears, reads once. A refused read leaves the facts unknown.
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
        // Refused: unknown, never a refusal.
        QVERIFY(perms.canSendMessages());
        QVERIFY(!perms.sendKnown());
        QVERIFY(perms.sendBlockedReason().isEmpty());
        QVERIFY(!perms.canRedactOthers());
        QVERIFY(!perms.redactKnown());
        // No open room: the room list changing asks nothing.
        perms.setRoomId(QString());
        client.setRoom(kRoom, false, true);
        QCOMPARE(client.asked.size(), 2);
    }
};

QTEST_GUILESS_MAIN(OpenRoomPermissionsTest)
#include "OpenRoomPermissionsTest.moc"
