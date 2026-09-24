// Activity Center model: classification and honesty rules.
//   * mentions, replies to the user's messages, replies in the user's
//     threads, reactions to the user's messages, invites and keyword hits
//     become entries; the user's own messages, local echoes, state rows,
//     redactions and thread-timeline copies never do;
//   * a reaction counts only when its target is the user's message;
//   * keywords match whole words, case-insensitively;
//   * seen state is the model's own: the badge counts unseen entries, "mark
//     all seen" persists only a marker (never previews), and opening an entry
//     marks it seen and emits the navigation triple;
//   * the list is bounded and newest-first; account switch or sign-out drops
//     everything.
// Uses a fake client emitting the Rust backend's signals; a live homeserver,
// the /notifications seed and the panel's rendering are not tested here.

#include "matrix/MatrixClient.h"
#include "models/ActivityModel.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;
    QString self = QStringLiteral("@me:mock.local");
    QList<RoomInfo> roomSet;
    // Only the Rust backend keeps all four unread fields current; one case
    // turns this off to prove the refusal is real.
    bool tracksRead = true;
    bool tracksRoomReadState() const override { return tracksRead; }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return self; }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return roomSet; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    // A real client answers the user id for a member whose snapshot has not
    // landed (MatrixClient's documented fallback), never "". `displayNames`
    // lets a case model before and after.
    QHash<QString, QString> displayNames;
    QString displayNameFor(const QString &, const QString &id) const override
    { return displayNames.value(id, id); }
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
};

const QString kRoom = QStringLiteral("!room:mock.local");

TimelineEvent text(const QString &eventId, const QString &sender, const QString &body,
                   qint64 tsMs)
{
    TimelineEvent e;
    e.eventId = eventId;
    e.roomId = kRoom;
    e.sender = sender;
    e.senderDisplayName = sender.mid(1, sender.indexOf(QLatin1Char(':')) - 1);
    e.body = body;
    e.type = TimelineEvent::TextMessage;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(tsMs);
    return e;
}

struct Harness {
    FakeClient client;
    ActivityModel model;
    QVariantMap stored;
    int saves = 0;

    Harness()
    {
        model.setStore({ [this] { return stored; },
                         [this](const QVariantMap &m) { stored = m; ++saves; } });
        model.setClient(&client);
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    }
    QVariantMap row(int i) const { return model.entryAt(i); }
};

} // namespace

class ActivityModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Reading a room (this client's read receipt) marks its entries seen up
    // to that point.
    void readingARoomMarksItsEntriesSeenUpToThatPoint()
    {
        Harness h;
        TimelineEvent older = text(QStringLiteral("$r1"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me one"), 1000);
        older.mentionsMe = true;
        TimelineEvent newer = text(QStringLiteral("$r2"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me two"), 3000);
        newer.mentionsMe = true;
        QVERIFY(h.model.ingest(older, QStringLiteral("Lounge")));
        QVERIFY(h.model.ingest(newer, QStringLiteral("Lounge")));
        QCOMPARE(h.model.unseenCount(), 2);

        // Read up to the first only; the later mention stays unread.
        h.model.markRoomReadUpTo(kRoom, 1000);
        QCOMPARE(h.model.unseenCount(), 1);

        // A receipt in another room clears nothing here.
        h.model.markRoomReadUpTo(QStringLiteral("!other:mock.local"), 9000);
        QCOMPARE(h.model.unseenCount(), 1);

        // Reading past the second clears the bell.
        h.model.markRoomReadUpTo(kRoom, 3000);
        QCOMPARE(h.model.unseenCount(), 0);

        // The rows remain, seen rather than deleted: the panel is a history.
        QCOMPARE(h.model.count(), 2);
        QCOMPARE(h.row(0).value(QStringLiteral("seen")).toBool(), true);
    }

    // A read on another device clears the bell. This client sends no receipt
    // then, so the signal is the room's own unread state, which the SDK
    // derives from the user's receipt on any device.
    void anotherDevicesReadClearsTheBell()
    {
        Harness h;
        TimelineEvent one = text(QStringLiteral("$p1"),
                                 QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("hey @me one"), 1000);
        one.mentionsMe = true;
        TimelineEvent two = text(QStringLiteral("$p2"),
                                 QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("hey @me two"), 3000);
        two.mentionsMe = true;
        QVERIFY(h.model.ingest(one, QStringLiteral("Lounge")));
        QVERIFY(h.model.ingest(two, QStringLiteral("Lounge")));
        QCOMPARE(h.model.unseenCount(), 2);

        // Still unread: one highlight outstanding.
        RoomInfo busy;
        busy.id = kRoom;
        busy.name = QStringLiteral("Lounge");
        busy.raiseActivity(QDateTime::fromMSecsSinceEpoch(3000));
        busy.highlightCount = 1;
        busy.hasUnreadMessages = true;
        h.client.roomSet = { busy };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 2);

        // A zero count while the SDK still reports unread messages is not
        // "read" either.
        RoomInfo half = busy;
        half.highlightCount = 0;
        h.client.roomSet = { half };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 2);

        // Read elsewhere; no receipt of our own.
        RoomInfo read = half;
        read.hasUnreadMessages = false;
        read.unreadCount = 0;
        h.client.roomSet = { read };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 0);
        // History, not a queue: the rows survive, marked seen.
        QCOMPARE(h.model.count(), 2);
        QCOMPARE(h.row(0).value(QStringLiteral("seen")).toBool(), true);
    }

    // A fresh mention in an already-read room must survive its own arrival:
    // eventAppended creates the row, then roomUpdated raises lastActivity to
    // the event's timestamp while the unread fields are still stale. Neither
    // signal carries unread state, so neither may clear the row.
    void aMentionIsNotClearedByTheUpdateThatAnnouncesIt()
    {
        Harness h;
        // The room before the mention: read, with lastActivity already at the
        // mention's timestamp, as the timeline path leaves it.
        RoomInfo stale;
        stale.id = kRoom;
        stale.name = QStringLiteral("Lounge");
        stale.raiseActivity(QDateTime::fromMSecsSinceEpoch(5000));
        h.client.roomSet = { stale };

        TimelineEvent fresh = text(QStringLiteral("$fresh"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me"), 5000);
        fresh.mentionsMe = true;
        QVERIFY(h.model.ingest(fresh, QStringLiteral("Lounge")));
        QCOMPARE(h.model.unseenCount(), 1);

        // Neither timeline-path signal carries an unread field.
        Q_EMIT h.client.roomUpdated(kRoom);
        QCOMPARE(h.model.unseenCount(), 1);
        // A caught-up payload says unread, so the batch path keeps it too.
        RoomInfo caughtUp = stale;
        caughtUp.highlightCount = 1;
        caughtUp.hasUnreadMessages = true;
        h.client.roomSet = { caughtUp };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);
    }

    // A backend that never writes hasUnreadMessages/markedUnread is not
    // saying "read"; notification_count alone is not a read signal.
    void aBackendThatDoesNotTrackReadStateClearsNothing()
    {
        Harness h;
        h.client.tracksRead = false;
        TimelineEvent e = text(QStringLiteral("$q1"),
                               QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me"), 1000);
        e.mentionsMe = true;
        QVERIFY(h.model.ingest(e, QStringLiteral("Lounge")));

        RoomInfo silent;
        silent.id = kRoom;
        silent.name = QStringLiteral("Lounge");
        silent.raiseActivity(QDateTime::fromMSecsSinceEpoch(9000));
        h.client.roomSet = { silent };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);

        // The same state on a tracking backend clears it, so the case above is
        // a real refusal.
        h.client.tracksRead = true;
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // Refusals that keep the cross-device path from over-claiming.
    void aClearRoomNeverClearsWhatItCannotSpeakFor()
    {
        Harness h;
        // An entry newer than the room's latest activity cannot have been read.
        TimelineEvent future = text(QStringLiteral("$f1"),
                                    QStringLiteral("@bob:mock.local"),
                                    QStringLiteral("hey @me later"), 9000);
        future.mentionsMe = true;
        QVERIFY(h.model.ingest(future, QStringLiteral("Lounge")));

        RoomInfo read;
        read.id = kRoom;
        read.name = QStringLiteral("Lounge");
        read.raiseActivity(QDateTime::fromMSecsSinceEpoch(3000));
        h.client.roomSet = { read };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);

        // Marked unread by the user: that beats the receipt.
        RoomInfo flagged = read;
        flagged.raiseActivity(QDateTime::fromMSecsSinceEpoch(9000));
        flagged.markedUnread = true;
        h.client.roomSet = { flagged };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);

        // A genuinely read room does clear it, so the case above is a refusal.
        RoomInfo clear = flagged;
        clear.markedUnread = false;
        h.client.roomSet = { clear };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // An invite is not a message: a room with nothing to read must not clear
    // it. The room also holds an ordinary unseen row, and its lastActivity is
    // later than both, so only the marking loop's invite skip can save it.
    void aClearRoomNeverClearsAnInvite()
    {
        Harness h;
        const QString roomId = QStringLiteral("!invited:mock.local");
        RoomInfo invited;
        invited.id = roomId;
        invited.name = QStringLiteral("Secret Club");
        invited.membership = RoomInfo::Invited;
        QVERIFY(h.model.noteInvite(invited));

        TimelineEvent mention = text(QStringLiteral("$i1"),
                                     QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("hey @me"), 1000);
        mention.roomId = roomId;
        mention.mentionsMe = true;
        QVERIFY(h.model.ingest(mention, QStringLiteral("Secret Club")));
        QCOMPARE(h.model.unseenCount(), 2);

        invited.raiseActivity(QDateTime::currentDateTime().addSecs(60));
        h.client.roomSet = { invited };
        Q_EMIT h.client.roomsChanged();
        // The ordinary row cleared; the invite did not.
        QCOMPARE(h.model.unseenCount(), 1);
        // The invite survived, not the mention; rows are newest-first and
        // noteInvite stamps "now", so identify it by kind.
        QString unseenKind;
        for (int i = 0; i < h.model.count(); ++i) {
            const QVariantMap row = h.model.entryAt(i);
            if (!row.value(QStringLiteral("seen")).toBool())
                unseenKind = row.value(QStringLiteral("kind")).toString();
        }
        QCOMPARE(unseenKind, QStringLiteral("invite"));

        // Answering it removes it.
        h.model.inviteResolved(roomId);
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // Refusals for receipts that cannot be compared.
    void readingARoomNeverMarksAnEntryItCannotCompare()
    {
        Harness h;
        TimelineEvent e = text(QStringLiteral("$r3"),
                               QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me"), 5000);
        e.mentionsMe = true;
        QVERIFY(h.model.ingest(e, QStringLiteral("Lounge")));
        QCOMPARE(h.model.unseenCount(), 1);

        // A receipt OLDER than the entry leaves it alone.
        h.model.markRoomReadUpTo(kRoom, 4999);
        QCOMPARE(h.model.unseenCount(), 1);
        // A zero or negative timestamp is not an answer.
        h.model.markRoomReadUpTo(kRoom, 0);
        QCOMPARE(h.model.unseenCount(), 1);
        // Neither is an empty room id.
        h.model.markRoomReadUpTo(QString(), 9000);
        QCOMPARE(h.model.unseenCount(), 1);
    }

    void mentionsBecomeEntriesAndCountAsUnseen()
    {
        Harness h;
        TimelineEvent e = text(QStringLiteral("$m1"), QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me look"), 1000);
        e.mentionsMe = true;
        QVERIFY(h.model.ingest(e, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.model.unseenCount(), 1);
        const QVariantMap r = h.row(0);
        QCOMPARE(r.value(QStringLiteral("kind")).toString(), QStringLiteral("mention"));
        QCOMPARE(r.value(QStringLiteral("roomName")).toString(), QStringLiteral("Lounge"));
        QCOMPARE(r.value(QStringLiteral("senderName")).toString(), QStringLiteral("bob"));
        QCOMPARE(r.value(QStringLiteral("preview")).toString(),
                 QStringLiteral("hey @me look"));
        QCOMPARE(r.value(QStringLiteral("seen")).toBool(), false);
        // @room is a mention too, but ranked below a direct one.
        TimelineEvent room = text(QStringLiteral("$m2"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("@room meeting"), 2000);
        room.mentionsRoom = true;
        QVERIFY(h.model.ingest(room, QStringLiteral("Lounge")));
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("room_mention"));
        // The same event twice is one entry.
        QVERIFY(!h.model.ingest(e, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 2);
    }

    void ownMessagesEchoesStateRowsAndRedactionsAreNeverActivity()
    {
        Harness h;
        TimelineEvent mine = text(QStringLiteral("$own"), h.client.self,
                                  QStringLiteral("@me talking to myself"), 1000);
        mine.mentionsMe = true;
        QVERIFY(!h.model.ingest(mine, QStringLiteral("Lounge")));
        TimelineEvent echo = text(QStringLiteral("$echo"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("@me"), 1001);
        echo.mentionsMe = true;
        echo.isLocalEcho = true;
        QVERIFY(!h.model.ingest(echo, QStringLiteral("Lounge")));
        TimelineEvent state = text(QStringLiteral("$state"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("@me joined"), 1002);
        state.mentionsMe = true;
        state.type = TimelineEvent::StateChange;
        QVERIFY(!h.model.ingest(state, QStringLiteral("Lounge")));
        TimelineEvent gone = text(QStringLiteral("$gone"), QStringLiteral("@bob:mock.local"),
                                  QString(), 1003);
        gone.mentionsMe = true;
        gone.redacted = true;
        QVERIFY(!h.model.ingest(gone, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 0);
        QCOMPARE(h.model.unseenCount(), 0);
    }

    void repliesToMyMessagesAndRepliesInMyThreads()
    {
        Harness h;
        // My own message is remembered (not listed).
        QVERIFY(!h.model.ingest(text(QStringLiteral("$mine"), h.client.self,
                                     QStringLiteral("question?"), 1000),
                                QStringLiteral("Lounge")));
        TimelineEvent reply = text(QStringLiteral("$r1"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("answer"), 2000);
        reply.replyToEventId = QStringLiteral("$mine");
        QVERIFY(h.model.ingest(reply, QStringLiteral("Lounge")));
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(), QStringLiteral("reply"));
        // A reply whose target sender is me, even if the target predates this
        // session.
        TimelineEvent reply2 = text(QStringLiteral("$r2"), QStringLiteral("@bob:mock.local"),
                                    QStringLiteral("also"), 2100);
        reply2.replyToEventId = QStringLiteral("$ancient");
        reply2.replyToSenderId = h.client.self;
        QVERIFY(h.model.ingest(reply2, QStringLiteral("Lounge")));
        // A reply to somebody else is nothing.
        TimelineEvent other = text(QStringLiteral("$r3"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("no"), 2200);
        other.replyToEventId = QStringLiteral("$carol");
        other.replyToSenderId = QStringLiteral("@carol:mock.local");
        QVERIFY(!h.model.ingest(other, QStringLiteral("Lounge")));

        // A thread rooted at my message: replies are "thread" activity.
        TimelineEvent root = text(QStringLiteral("$root"), h.client.self,
                                  QStringLiteral("thread start"), 3000);
        root.isThreadRoot = true;
        QVERIFY(!h.model.ingest(root, QStringLiteral("Lounge")));
        TimelineEvent inThread = text(QStringLiteral("$t1"), QStringLiteral("@bob:mock.local"),
                                      QStringLiteral("in your thread"), 3100);
        inThread.threadRootId = QStringLiteral("$root");
        QVERIFY(h.model.ingest(inThread, QStringLiteral("Lounge")));
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(), QStringLiteral("thread"));
        QCOMPARE(h.row(0).value(QStringLiteral("threadRootId")).toString(),
                 QStringLiteral("$root"));
        // A thread I merely replied in counts as mine too.
        TimelineEvent myReply = text(QStringLiteral("$t2"), h.client.self,
                                     QStringLiteral("me in bob's thread"), 3200);
        myReply.threadRootId = QStringLiteral("$bobroot");
        QVERIFY(!h.model.ingest(myReply, QStringLiteral("Lounge")));
        TimelineEvent later = text(QStringLiteral("$t3"), QStringLiteral("@carol:mock.local"),
                                   QStringLiteral("carol in bob's thread"), 3300);
        later.threadRootId = QStringLiteral("$bobroot");
        QVERIFY(h.model.ingest(later, QStringLiteral("Lounge")));
        // A thread I never touched is not.
        TimelineEvent foreign = text(QStringLiteral("$t4"), QStringLiteral("@carol:mock.local"),
                                     QStringLiteral("elsewhere"), 3400);
        foreign.threadRootId = QStringLiteral("$unknownroot");
        QVERIFY(!h.model.ingest(foreign, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 4);
    }

    void keywordsMatchWholeWordsCaseInsensitively()
    {
        QVERIFY(ActivityModel::matchesKeyword(QStringLiteral("The Lightning build"),
                                              QStringLiteral("lightning")));
        QVERIFY(!ActivityModel::matchesKeyword(QStringLiteral("enlightning"),
                                               QStringLiteral("lightning")));
        QVERIFY(!ActivityModel::matchesKeyword(QStringLiteral("lightnings"),
                                               QStringLiteral("lightning")));
        QVERIFY(ActivityModel::matchesKeyword(QStringLiteral("see #lightning now"),
                                              QStringLiteral("#lightning")));
        QVERIFY(ActivityModel::matchesKeyword(QStringLiteral("Ąžuolas here"),
                                              QStringLiteral("ąžuolas")));
        QVERIFY(!ActivityModel::matchesKeyword(QStringLiteral("anything"), QString()));

        Harness h;
        h.model.setKeywords({ QStringLiteral("deploy"), QStringLiteral(" Deploy "),
                              QString(), QStringLiteral("release") });
        QCOMPARE(h.model.keywords(), (QStringList{ QStringLiteral("deploy"),
                                                   QStringLiteral("release") }));
        QCOMPARE(h.stored.value(QStringLiteral("keywords")).toStringList().size(), 2);
        QVERIFY(h.model.ingest(text(QStringLiteral("$k1"), QStringLiteral("@bob:mock.local"),
                                    QStringLiteral("DEPLOY at noon"), 1000),
                               QStringLiteral("Ops")));
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(), QStringLiteral("keyword"));
        QVERIFY(!h.model.ingest(text(QStringLiteral("$k2"), QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("redeployment"), 1100),
                                QStringLiteral("Ops")));
        // An undecryptable event has no body and is never a keyword hit.
        TimelineEvent enc = text(QStringLiteral("$k3"), QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("deploy"), 1200);
        enc.undecryptable = true;
        QVERIFY(!h.model.ingest(enc, QStringLiteral("Ops")));
        // ...but an undecryptable mention still lists, without a preview.
        enc.mentionsMe = true;
        QVERIFY(h.model.ingest(enc, QStringLiteral("Ops")));
        QCOMPARE(h.row(0).value(QStringLiteral("preview")).toString(), QString());
        QCOMPARE(h.row(0).value(QStringLiteral("encrypted")).toBool(), true);
    }

    void reactionsCountOnlyWhenTheTargetIsMine()
    {
        Harness h;
        QVERIFY(!h.model.ingest(text(QStringLiteral("$mine"), h.client.self,
                                     QStringLiteral("joke"), 1000),
                                QStringLiteral("Lounge")));
        QVERIFY(h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                     QStringLiteral("$react1"), QStringLiteral("$mine"),
                                     QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("bob"), QStringLiteral("😂"), 2000));
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("reaction"));
        QCOMPARE(h.row(0).value(QStringLiteral("reactionKey")).toString(),
                 QStringLiteral("😂"));
        // Navigation targets the reacted-to MESSAGE, not the reaction.
        QCOMPARE(h.row(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$mine"));
        QVERIFY(!h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                      QStringLiteral("$react2"), QStringLiteral("$bobs"),
                                      QStringLiteral("@carol:mock.local"), QString(),
                                      QStringLiteral("👍"), 2100));
        // My own reaction to my own message is not activity.
        QVERIFY(!h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                      QStringLiteral("$react3"), QStringLiteral("$mine"),
                                      h.client.self, QString(), QStringLiteral("👍"), 2200));
        QCOMPARE(h.model.count(), 1);
    }

    void invitesListOnceAndLeaveWhenAnswered()
    {
        Harness h;
        RoomInfo r;
        r.id = QStringLiteral("!inv:mock.local");
        r.name = QStringLiteral("Secret club");
        r.membership = RoomInfo::Invited;
        r.inviterUserId = QStringLiteral("@bob:mock.local");
        QVERIFY(h.model.noteInvite(r));
        QVERIFY(!h.model.noteInvite(r));
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(), QStringLiteral("invite"));
        QCOMPARE(h.row(0).value(QStringLiteral("eventId")).toString(), QString());
        r.membership = RoomInfo::Joined;
        QVERIFY(!h.model.noteInvite(r));
        h.model.inviteResolved(r.id);
        QCOMPARE(h.model.count(), 0);
    }

    void seenStateIsOwnedByTheModelAndOnlyTheMarkerIsPersisted()
    {
        Harness h;
        for (int i = 1; i <= 3; ++i) {
            TimelineEvent e = text(QStringLiteral("$m%1").arg(i),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("secret body %1").arg(i), 1000 * i);
            e.mentionsMe = true;
            h.model.ingest(e, QStringLiteral("Lounge"));
        }
        QCOMPARE(h.model.unseenCount(), 3);
        QSignalSpy open(&h.model, &ActivityModel::openRequested);
        h.model.open(QStringLiteral("$m2"));
        QCOMPARE(open.size(), 1);
        QCOMPARE(open.first().at(0).toString(), kRoom);
        QCOMPARE(open.first().at(1).toString(), QStringLiteral("$m2"));
        QCOMPARE(h.model.unseenCount(), 2);
        h.model.markAllSeen();
        QCOMPARE(h.model.unseenCount(), 0);
        QVERIFY(h.saves >= 1);
        // The store holds only the marker, nothing from a message.
        QCOMPARE(h.stored.value(QStringLiteral("seenUpToMs")).toLongLong(), 3000);
        for (auto it = h.stored.cbegin(); it != h.stored.cend(); ++it)
            QVERIFY2(!it.value().toString().contains(QStringLiteral("secret")),
                     qPrintable(it.key()));
        QVERIFY(!h.stored.contains(QStringLiteral("entries")));
        // A newer entry is unseen again; an older straggler is covered.
        TimelineEvent newer = text(QStringLiteral("$m9"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("later"), 9000);
        newer.mentionsMe = true;
        h.model.ingest(newer, QStringLiteral("Lounge"));
        TimelineEvent older = text(QStringLiteral("$m0"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("earlier"), 500);
        older.mentionsMe = true;
        h.model.ingest(older, QStringLiteral("Lounge"));
        QCOMPARE(h.model.unseenCount(), 1);
        // Newest first regardless of arrival order.
        QCOMPARE(h.row(0).value(QStringLiteral("entryId")).toString(), QStringLiteral("$m9"));
        QCOMPARE(h.row(h.model.count() - 1).value(QStringLiteral("entryId")).toString(),
                 QStringLiteral("$m0"));
    }

    void theMarkerIsLoadedForTheAccountOnConnect()
    {
        FakeClient client;
        ActivityModel model;
        QVariantMap stored{ { QStringLiteral("seenUpToMs"), 5000 },
                            { QStringLiteral("keywords"), QStringList{ QStringLiteral("ops") } } };
        model.setStore({ [&] { return stored; }, [&](const QVariantMap &m) { stored = m; } });
        model.setClient(&client);
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
        QCOMPARE(model.keywords(), QStringList{ QStringLiteral("ops") });
        TimelineEvent old = text(QStringLiteral("$old"), QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("ops"), 4000);
        model.ingest(old, QStringLiteral("Ops"));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.unseenCount(), 0);
    }

    void filtersNarrowTheRowsWithoutTouchingTheBadge()
    {
        Harness h;
        TimelineEvent m = text(QStringLiteral("$m"), QStringLiteral("@bob:mock.local"),
                               QStringLiteral("@me"), 1000);
        m.mentionsMe = true;
        h.model.ingest(m, QStringLiteral("Lounge"));
        RoomInfo r;
        r.id = QStringLiteral("!inv:mock.local");
        r.membership = RoomInfo::Invited;
        h.model.noteInvite(r);
        QCOMPARE(h.model.count(), 2);
        h.model.setFilter(QStringLiteral("invites"));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(), QStringLiteral("invite"));
        QCOMPARE(h.model.unseenCount(), 2);
        h.model.setFilter(QStringLiteral("nonsense"));
        QCOMPARE(h.model.filter(), QStringLiteral("all"));
        QCOMPARE(h.model.count(), 2);
        // An entry arriving while a filter hides it still counts.
        h.model.setFilter(QStringLiteral("invites"));
        TimelineEvent m2 = text(QStringLiteral("$m2"), QStringLiteral("@bob:mock.local"),
                                QStringLiteral("@me again"), 2000);
        m2.mentionsMe = true;
        h.model.ingest(m2, QStringLiteral("Lounge"));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.model.unseenCount(), 3);
        h.model.setFilter(QStringLiteral("all"));
        QCOMPARE(h.model.count(), 3);
    }

    void theListIsBoundedNewestFirst()
    {
        Harness h;
        for (int i = 0; i < ActivityModel::kMaxEntries + 25; ++i) {
            TimelineEvent e = text(QStringLiteral("$b%1").arg(i),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("x"), 1000 + i);
            e.mentionsMe = true;
            h.model.ingest(e, QStringLiteral("Lounge"));
        }
        QCOMPARE(h.model.count(), ActivityModel::kMaxEntries);
        QCOMPARE(h.row(0).value(QStringLiteral("entryId")).toString(),
                 QStringLiteral("$b%1").arg(ActivityModel::kMaxEntries + 24));
        // The oldest were the ones dropped.
        QCOMPARE(h.row(h.model.count() - 1).value(QStringLiteral("entryId")).toString(),
                 QStringLiteral("$b25"));
    }

    void anAccountSwitchOrSignOutDropsEverything()
    {
        Harness h;
        TimelineEvent m = text(QStringLiteral("$m"), QStringLiteral("@bob:mock.local"),
                               QStringLiteral("@me"), 1000);
        m.mentionsMe = true;
        h.model.ingest(m, QStringLiteral("Lounge"));
        h.model.ingest(text(QStringLiteral("$mine"), h.client.self, QStringLiteral("x"), 1100),
                       QStringLiteral("Lounge"));
        Q_EMIT h.client.loggedOut();
        QCOMPARE(h.model.count(), 0);
        QCOMPARE(h.model.unseenCount(), 0);
        // The previous account's own-message memory is gone too.
        FakeClient other;
        other.self = QStringLiteral("@other:mock.local");
        h.model.setClient(&other);
        TimelineEvent reply = text(QStringLiteral("$r"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("re"), 2000);
        reply.replyToEventId = QStringLiteral("$mine");
        QVERIFY(!h.model.ingest(reply, QStringLiteral("Lounge")));
    }

    void serverSeedFillsAFreshSessionAndHonoursTheReadFlag()
    {
        Harness h;
        h.model.seed({
            QVariantMap{ { QStringLiteral("eventId"), QStringLiteral("$s1") },
                         { QStringLiteral("roomId"), kRoom },
                         { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                         { QStringLiteral("preview"), QStringLiteral("old mention") },
                         { QStringLiteral("timestampMs"), 100 },
                         { QStringLiteral("read"), true } },
            QVariantMap{ { QStringLiteral("eventId"), QStringLiteral("$s2") },
                         { QStringLiteral("roomId"), kRoom },
                         { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                         { QStringLiteral("timestampMs"), 200 },
                         { QStringLiteral("encrypted"), true } },
            // My own highlighted event (a @room I sent) is not activity.
            QVariantMap{ { QStringLiteral("eventId"), QStringLiteral("$s3") },
                         { QStringLiteral("roomId"), kRoom },
                         { QStringLiteral("senderId"), h.client.self },
                         { QStringLiteral("timestampMs"), 300 } },
        });
        QCOMPARE(h.model.count(), 2);
        QCOMPARE(h.model.unseenCount(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("entryId")).toString(), QStringLiteral("$s2"));
        QCOMPARE(h.row(0).value(QStringLiteral("encrypted")).toBool(), true);
        // A live copy of a seeded event does not duplicate it.
        TimelineEvent dup = text(QStringLiteral("$s1"), QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("old mention"), 100);
        dup.mentionsMe = true;
        QVERIFY(!h.model.ingest(dup, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 2);
    }

    // The bell must agree with the room list. The seed
    // (GET /notifications?only=highlight) and the room list (highlight_count)
    // can disagree; the room list wins, since it is what the user sees.
    void aRoomWithNoUnreadHighlightsContributesNothingToTheBell()
    {
        Harness h;
        RoomInfo quiet;
        quiet.id = kRoom;
        quiet.name = QStringLiteral("Lightning Support");
        quiet.highlightCount = 0;
        h.client.roomSet = { quiet };

        QVariantList rows;
        for (int i = 1; i <= 4; ++i) {
            rows.append(QVariantMap{
                { QStringLiteral("eventId"), QStringLiteral("$old%1").arg(i) },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), 100 * i },
                // As a live server sent them: every row flagged unread.
                { QStringLiteral("read"), false },
            });
        }
        h.model.seed(rows);
        QCOMPARE(h.model.count(), 4);
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // A room's unread budget goes to its newest rows; marking the oldest
    // unread would send the user to a message read long ago.
    void theRoomsUnreadBudgetGoesToItsNewestRows()
    {
        Harness h;
        RoomInfo busy;
        busy.id = kRoom;
        busy.name = QStringLiteral("Lightning Support");
        busy.highlightCount = 1;
        RoomInfo other;
        other.id = QStringLiteral("!other:mock.local");
        other.name = QStringLiteral("Sales");
        other.highlightCount = 0;
        h.client.roomSet = { busy, other };

        QVariantList rows;
        for (int i = 1; i <= 3; ++i) {
            rows.append(QVariantMap{
                { QStringLiteral("eventId"), QStringLiteral("$b%1").arg(i) },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), 100 * i },
                { QStringLiteral("read"), false },
            });
        }
        // A zero-count room does not borrow another room's budget.
        rows.append(QVariantMap{
            { QStringLiteral("eventId"), QStringLiteral("$o1") },
            { QStringLiteral("roomId"), other.id },
            { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
            { QStringLiteral("timestampMs"), 400 },
            { QStringLiteral("read"), false },
        });
        h.model.seed(rows);
        QCOMPARE(h.model.count(), 4);
        QCOMPARE(h.model.unseenCount(), 1);
        // Newest first: row 0 is the other room's 400 (seen); the unseen row
        // is $b3 at 300.
        int unseenRow = -1;
        for (int i = 0; i < h.model.count(); ++i) {
            if (!h.row(i).value(QStringLiteral("seen")).toBool())
                unseenRow = i;
        }
        QVERIFY(unseenRow >= 0);
        QCOMPARE(h.row(unseenRow).value(QStringLiteral("entryId")).toString(),
                 QStringLiteral("$b3"));
    }

    // An unknown room keeps the server's flag: roomInfo() returns a default
    // RoomInfo whose zero highlightCount means "unknown", not "nothing
    // unread".
    void anUnknownRoomsSeededRowsKeepTheServersFlag()
    {
        Harness h;
        h.client.roomSet = {};   // nothing known
        h.model.seed({
            QVariantMap{
                { QStringLiteral("eventId"), QStringLiteral("$u1") },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), 100 },
                { QStringLiteral("read"), false } },
        });
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.model.unseenCount(), 1);
    }

    // ---- seed placeholders are replaced when answers arrive ----

    // The seed runs on the first `Syncing` state, before any room payload, so
    // a seeded row must learn its room name when the room arrives.
    void aSeededRowLearnsItsRoomNameWhenTheRoomArrives()
    {
        Harness h;
        h.client.roomSet = {};      // the seed beats the first room payload
        h.model.seed({
            QVariantMap{
                { QStringLiteral("eventId"), QStringLiteral("$n1") },
                // roomInfo()'s fallback is an empty name, so AppController
                // really sends "" here (unlike the sender); both must pend.
                { QStringLiteral("roomName"), QString() },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), 100 },
                { QStringLiteral("read"), false } },
        });
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("roomName")).toString(), kRoom);

        QSignalSpy changed(&h.model, &QAbstractItemModel::dataChanged);
        RoomInfo late;
        late.id = kRoom;
        late.name = QStringLiteral("Design Review");
        late.highlightCount = 1;
        h.client.roomSet = { late };
        Q_EMIT h.client.roomsChanged();

        QCOMPARE(h.row(0).value(QStringLiteral("roomName")).toString(),
                 QStringLiteral("Design Review"));
        QVERIFY2(changed.count() > 0,
                 "the room name was replaced without notifying, so a bound "
                 "delegate would keep rendering the raw room id");
    }

    // Likewise the sender name, when /members lands after the seed.
    void aSeededRowLearnsItsSenderNameWhenTheMembersArrive()
    {
        Harness h;
        RoomInfo known;
        known.id = kRoom;
        known.name = QStringLiteral("Design Review");
        known.highlightCount = 1;
        h.client.roomSet = { known };
        h.client.displayNames.clear();    // /members has not landed
        // As AppController sends it: `senderName` pre-filled with
        // displayNameFor()'s MXID fallback, so the key is non-empty.
        h.model.seed({
            QVariantMap{
                { QStringLiteral("eventId"), QStringLiteral("$n2") },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("senderName"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), 100 },
                { QStringLiteral("read"), false } },
        });
        QCOMPARE(h.row(0).value(QStringLiteral("senderName")).toString(),
                 QStringLiteral("@bob:mock.local"));

        h.client.displayNames.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("Bob"));
        Q_EMIT h.client.membersChanged(kRoom);
        QCOMPARE(h.row(0).value(QStringLiteral("senderName")).toString(),
                 QStringLiteral("Bob"));
    }

    // A member snapshot for another room leaves this row alone.
    void anotherRoomsMembersLeaveThisRowAlone()
    {
        Harness h;
        h.model.seed({
            QVariantMap{
                { QStringLiteral("eventId"), QStringLiteral("$n3") },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), 100 },
                { QStringLiteral("read"), false } },
        });
        h.client.displayNames.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("Bob"));
        Q_EMIT h.client.membersChanged(QStringLiteral("!elsewhere:mock.local"));
        QCOMPARE(h.row(0).value(QStringLiteral("senderName")).toString(),
                 QStringLiteral("@bob:mock.local"));
    }

    // reconcileSeedAgainstRoomCounts() skips unknown rooms, which is every
    // room when the seed lands first, so it is retried once the room is known.
    void theSeedReconcileRetriesOnceItsRoomIsKnown()
    {
        Harness h;
        h.client.roomSet = {};
        QVariantList rows;
        for (int i = 1; i <= 3; ++i) {
            rows.append(QVariantMap{
                { QStringLiteral("eventId"), QStringLiteral("$late%1").arg(i) },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), 100 + i },
                { QStringLiteral("read"), false } });
        }
        h.model.seed(rows);
        // Nothing known: all three keep the server's flag.
        QCOMPARE(h.model.unseenCount(), 3);

        // The room arrives with one unread highlight: the newest keeps the
        // badge and the older two are read.
        RoomInfo late;
        late.id = kRoom;
        late.name = QStringLiteral("Design Review");
        late.highlightCount = 1;
        h.client.roomSet = { late };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);

        // Which row keeps the badge matters: m_entries is newest-first, so
        // row 0 is $late3.
        QCOMPARE(h.row(0).value(QStringLiteral("entryId")).toString(),
                 QStringLiteral("$late3"));
        QCOMPARE(h.row(0).value(QStringLiteral("seen")).toBool(), false);
        QCOMPARE(h.row(1).value(QStringLiteral("seen")).toBool(), true);
        QCOMPARE(h.row(2).value(QStringLiteral("seen")).toBool(), true);

        // The retry must not spend the budget twice: a second payload for the
        // same room changes nothing.
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("seen")).toBool(), false);
    }

    // A live mention without senderDisplayName also learns its sender name,
    // not only seeded rows.
    void aLiveMentionAlsoLearnsItsSenderName()
    {
        Harness h;
        h.client.displayNames.clear();
        TimelineEvent m = text(QStringLiteral("$live1"),
                               QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me"), 5000);
        m.senderDisplayName.clear();     // the roster has not landed
        m.mentionsMe = true;
        QVERIFY(h.model.ingest(m, QStringLiteral("Design Review")));
        QCOMPARE(h.row(0).value(QStringLiteral("senderName")).toString(),
                 QStringLiteral("@bob:mock.local"));

        h.client.displayNames.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("Bob"));
        Q_EMIT h.client.membersChanged(kRoom);
        QCOMPARE(h.row(0).value(QStringLiteral("senderName")).toString(),
                 QStringLiteral("Bob"));
    }
};

QTEST_GUILESS_MAIN(ActivityModelTest)
#include "ActivityModelTest.moc"
