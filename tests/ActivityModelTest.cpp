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
    // One case signs in late (an offline restore that never reaches Syncing).
    bool loggedIn = true;
    bool isLoggedIn() const override { return loggedIn; }
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
    // The model's local clock: the real one, moved forward on demand so an
    // arrival and a counter write are strictly ordered.
    qint64 skew = 0;

    Harness()
    {
        model.setClock([this] { return clockMs(); });
        model.setStore({ [this] { return stored; },
                         [this](const QVariantMap &m) { stored = m; ++saves; } });
        model.setClient(&client);
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    }
    qint64 clockMs() const { return QDateTime::currentMSecsSinceEpoch() + skew; }
    void advance(qint64 ms) { skew += ms; }
    // A room payload written NOW, after every row so far arrived: the only
    // kind whose counters may speak for those rows.
    RoomInfo fresh(RoomInfo r)
    {
        advance(5);
        r.countsWrittenAtMs = clockMs();
        advance(5);
        return r;
    }
    QVariantMap row(int i) const { return model.entryAt(i); }
    QString kindOf(const QString &id) const
    {
        for (int i = 0; i < model.count(); ++i)
            if (model.entryAt(i).value(QStringLiteral("entryId")).toString() == id)
                return model.entryAt(i).value(QStringLiteral("kind")).toString();
        return {};
    }
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
        h.client.roomSet = { h.fresh(read) };
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
        h.client.roomSet = { h.fresh(silent) };
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
        h.client.roomSet = { h.fresh(clear) };
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
        h.client.roomSet = { h.fresh(invited) };
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
        // As it really arrives: an own message is not a thread root when it is
        // sent; it becomes one only when somebody replies in a thread on it.
        // Old code required isThreadRoot here and listed nothing.
        TimelineEvent root = text(QStringLiteral("$root"), h.client.self,
                                  QStringLiteral("thread start"), 3000);
        root.isThreadRoot = false;
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
        // A newer entry is unseen again, and so is an older one that ARRIVES
        // after the click (a federation delay, a skewed sender clock): the
        // click covered what was listed then, not an origin_server_ts. Old
        // code compared the timestamp and marked the straggler seen on
        // arrival.
        TimelineEvent newer = text(QStringLiteral("$m9"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("later"), 9000);
        newer.mentionsMe = true;
        h.model.ingest(newer, QStringLiteral("Lounge"));
        TimelineEvent older = text(QStringLiteral("$m0"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("earlier"), 500);
        older.mentionsMe = true;
        h.model.ingest(older, QStringLiteral("Lounge"));
        QCOMPARE(h.model.unseenCount(), 2);
        // Newest first regardless of arrival order.
        QCOMPARE(h.row(0).value(QStringLiteral("entryId")).toString(), QStringLiteral("$m9"));
        QCOMPARE(h.row(h.model.count() - 1).value(QStringLiteral("entryId")).toString(),
                 QStringLiteral("$m0"));

        // Across a restart the persisted marker still covers history the
        // previous session showed: the same rows re-delivered are seen.
        QVariantMap stored = h.stored;
        FakeClient client;
        ActivityModel next;
        next.setStore({ [&] { return stored; }, [&](const QVariantMap &m) { stored = m; } });
        next.setClient(&client);
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
        TimelineEvent again = text(QStringLiteral("$m2"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("secret body 2"), 2000);
        again.mentionsMe = true;
        QVERIFY(next.ingest(again, QStringLiteral("Lounge")));
        QCOMPARE(next.unseenCount(), 0);
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

    // RomanticAnimeGerl: the bell kept its badge after the room was read. A
    // room marked read without a timeline (menu, notification action, Mark
    // space read) sends no timeline receipt, so nothing cleared its rows.
    // Old code: markRoomReadNow does not exist.
    void aRoomMarkedReadWithoutATimelineClearsItsRows()
    {
        Harness h;
        TimelineEvent m = text(QStringLiteral("$n1"),
                               QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me"),
                               QDateTime::currentMSecsSinceEpoch() - 5000);
        m.mentionsMe = true;
        QVERIFY(h.model.ingest(m, QStringLiteral("Lounge")));
        QCOMPARE(h.model.unseenCount(), 1);
        h.model.markRoomReadNow(QStringLiteral("!other:mock.local"));
        QCOMPARE(h.model.unseenCount(), 1);
        h.model.markRoomReadNow(kRoom);
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // A thread's own receipt is threaded and clears that thread only. The main
    // receipt is UNTHREADED, so the server treats earlier thread replies as read
    // and the bell clears them up to the same point, and no further.
    void aThreadReceiptClearsItsThreadAndAMainReceiptClearsTheRoomUpToItsPoint()
    {
        Harness h;
        TimelineEvent main = text(QStringLiteral("$t0"),
                                  QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("hey @me main"), 1000);
        main.mentionsMe = true;
        TimelineEvent reply = text(QStringLiteral("$t1"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me thread"), 2000);
        reply.mentionsMe = true;
        reply.threadRootId = QStringLiteral("$root");
        QVERIFY(h.model.ingest(main, QStringLiteral("Lounge")));
        QVERIFY(h.model.ingest(reply, QStringLiteral("Lounge")));
        QCOMPARE(h.model.unseenCount(), 2);

        h.model.markRoomReadUpTo(kRoom, 9000, QStringLiteral("$other"));
        QCOMPARE(h.model.unseenCount(), 2);                  // another thread
        h.model.markRoomReadUpTo(kRoom, 1999, QStringLiteral("$root"));
        QCOMPARE(h.model.unseenCount(), 2);                  // before the reply
        h.model.markRoomReadUpTo(kRoom, 2000, QStringLiteral("$root"));
        QCOMPARE(h.model.unseenCount(), 1);                  // main row stays
        h.model.markRoomReadUpTo(kRoom, 1000);
        QCOMPARE(h.model.unseenCount(), 0);
    }

    void aMainReceiptStopsAtItsPointForThreadRowsToo()
    {
        Harness h;
        TimelineEvent reply = text(QStringLiteral("$t1"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me thread"), 2000);
        reply.mentionsMe = true;
        reply.threadRootId = QStringLiteral("$root");
        QVERIFY(h.model.ingest(reply, QStringLiteral("Lounge")));
        h.model.markRoomReadUpTo(kRoom, 1500);
        QCOMPARE(h.model.unseenCount(), 1);
        h.model.markRoomReadUpTo(kRoom, 2500);
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // A muted room is silent everywhere (as Element): its rows are hidden from
    // the bell and its badge, not dropped, so unmuting brings them back.
    void aMutedRoomIsHiddenFromTheBellAndTheBadgeUntilUnmuted()
    {
        Harness h;
        int mode = 0;
        h.model.setNotificationModeSource(
            [&mode](const QString &) { return mode; });
        TimelineEvent mention = text(QStringLiteral("$m1"),
                                     QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("hey @me"),
                                     QDateTime::currentMSecsSinceEpoch() - 5000);
        mention.mentionsMe = true;
        QVERIFY(h.model.ingest(mention, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.model.unseenCount(), 1);

        mode = 2;   // muted: the row is hidden and uncounted
        h.model.notificationModesChanged();
        QCOMPARE(h.model.count(), 0);
        QCOMPARE(h.model.unseenCount(), 0);

        // A new mention in the muted room is kept but hidden too.
        TimelineEvent again = mention;
        again.eventId = QStringLiteral("$m2");
        again.timestamp = QDateTime::currentDateTime();
        QVERIFY(h.model.ingest(again, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 0);
        QCOMPARE(h.model.unseenCount(), 0);

        mode = 0;   // unmuted: both rows are back and counted
        h.model.notificationModesChanged();
        QCOMPARE(h.model.count(), 2);
        QCOMPARE(h.model.unseenCount(), 2);
    }

    void markingARoomReadClearsItsThreadRowsToo()
    {
        Harness h;
        TimelineEvent reply = text(QStringLiteral("$t1"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me thread"),
                                   QDateTime::currentMSecsSinceEpoch() - 5000);
        reply.mentionsMe = true;
        reply.threadRootId = QStringLiteral("$root");
        QVERIFY(h.model.ingest(reply, QStringLiteral("Lounge")));
        h.model.markRoomReadNow(kRoom);
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // The two receipts that clear the bell have to be wired to it.
    void theReceiptsAreWiredToTheBell()
    {
        QFile f(QStringLiteral(SOURCE_DIR "/src/app/AppController.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(f.readAll());
        QVERIFY(src.contains(QStringLiteral(
            "ThreadController::threadReadSent")));
        QVERIFY(src.contains(QStringLiteral("RoomListModel::roomMarkedRead")));
        QVERIFY(src.contains(QStringLiteral("markRoomReadNow(roomId)")));
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

    // ---- 2026-10-15 notification sweep (package P4) ----
    // Each case names the finding and why it fails on the old model.

    // Findings 0/128. In an encrypted room the SDK's client counts skip thread
    // events and there is no server count, so a thread mention leaves the room
    // "clear" while lastActivity has moved past it. Old code marked the row
    // seen on the next roomsChanged; a thread row there now waits for a
    // receipt.
    void anEncryptedThreadMentionSurvivesAClearRoomPayload()
    {
        Harness h;
        TimelineEvent reply = text(QStringLiteral("$te1"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me in a thread"), 5000);
        reply.mentionsMe = true;
        reply.threadRootId = QStringLiteral("$root");
        QVERIFY(h.model.ingest(reply, QStringLiteral("Lounge")));

        RoomInfo room;
        room.id = kRoom;
        room.name = QStringLiteral("Lounge");
        room.encrypted = true;
        room.raiseActivity(QDateTime::fromMSecsSinceEpoch(5000));
        h.client.roomSet = { h.fresh(room) };   // counts 0, written after it
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);

        // Encryption not yet known fails closed the same way.
        room.encrypted = false;
        room.encryptionKnown = false;
        h.client.roomSet = { h.fresh(room) };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 1);

        // The thread's own receipt clears it.
        h.model.markRoomReadUpTo(kRoom, 5000, QStringLiteral("$root"));
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // The exemption is for encrypted rooms only: an unencrypted room's server
    // count includes thread events, so its clear state still speaks.
    void anUnencryptedClearRoomStillClearsItsThreadRows()
    {
        Harness h;
        TimelineEvent reply = text(QStringLiteral("$tu1"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me in a thread"), 5000);
        reply.mentionsMe = true;
        reply.threadRootId = QStringLiteral("$root");
        QVERIFY(h.model.ingest(reply, QStringLiteral("Lounge")));
        RoomInfo room;
        room.id = kRoom;
        room.name = QStringLiteral("Lounge");
        room.raiseActivity(QDateTime::fromMSecsSinceEpoch(5000));
        h.client.roomSet = { h.fresh(room) };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // Finding 4. roomsChanged is not per room: a payload for ANOTHER room
    // re-runs the sweep over this one, whose counters were written before the
    // mention arrived and still read 0. Old code marked the mention seen on
    // arrival.
    void aMentionBeforeAStaleCounterPayloadStaysUnseen()
    {
        Harness h;
        RoomInfo stale;
        stale.id = kRoom;
        stale.name = QStringLiteral("Lounge");
        stale.countsWrittenAtMs = h.clockMs();          // before the mention
        stale.raiseActivity(QDateTime::fromMSecsSinceEpoch(5000));
        h.client.roomSet = { stale };
        h.advance(5);

        TimelineEvent fresh = text(QStringLiteral("$st1"),
                                   QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("hey @me"), 5000);
        fresh.mentionsMe = true;
        QVERIFY(h.model.ingest(fresh, QStringLiteral("Lounge")));
        h.advance(5);
        Q_EMIT h.client.roomsChanged();                 // another room's diff
        QCOMPARE(h.model.unseenCount(), 1);

        // The same clear state written AFTER the row does clear it, so the
        // case above is the gate and not some other refusal.
        h.client.roomSet = { h.fresh(stale) };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // Finding 3. A reaction is not a timeline row: the receipt lands on the
    // newest message and the reaction (to an older one) is newer than it.
    // Old code compared the reaction's own timestamp and never cleared it.
    void aReactionRowIsClearedByReadingTheRoom()
    {
        Harness h;
        QVERIFY(!h.model.ingest(text(QStringLiteral("$mine"), h.client.self,
                                     QStringLiteral("joke"), 1000),
                                QStringLiteral("Lounge")));
        QVERIFY(h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                     QStringLiteral("$react1"), QStringLiteral("$mine"),
                                     QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("bob"), QStringLiteral("🎉"), 5000));
        QCOMPARE(h.model.unseenCount(), 1);
        // A threaded receipt says nothing about it.
        h.advance(10);
        h.model.markRoomReadUpTo(kRoom, 2000, QStringLiteral("$someroot"));
        QCOMPARE(h.model.unseenCount(), 1);
        // The room's own receipt, on a message older than the reaction.
        h.model.markRoomReadUpTo(kRoom, 2000);
        QCOMPARE(h.model.unseenCount(), 0);

        // A reaction arriving AFTER that read is still news.
        QVERIFY(h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                     QStringLiteral("$react2"), QStringLiteral("$mine"),
                                     QStringLiteral("@carol:mock.local"),
                                     QStringLiteral("carol"), QStringLiteral("👍"), 6000));
        QCOMPARE(h.model.unseenCount(), 1);
    }

    // Finding 1. A deleted message or a withdrawn reaction leaves the list,
    // preview and all; a late copy cannot bring it back. Old code had no
    // removal path at all.
    void aRedactionRemovesTheRow()
    {
        Harness h;
        TimelineEvent m = text(QStringLiteral("$del1"), QStringLiteral("@bob:mock.local"),
                               QStringLiteral("something private @me"), 1000);
        m.mentionsMe = true;
        QVERIFY(h.model.ingest(m, QStringLiteral("Lounge")));
        QVERIFY(!h.model.ingest(text(QStringLiteral("$mine"), h.client.self,
                                     QStringLiteral("joke"), 900),
                                QStringLiteral("Lounge")));
        QVERIFY(h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                     QStringLiteral("$re1"), QStringLiteral("$mine"),
                                     QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("bob"), QStringLiteral("👍"), 2000));
        QCOMPARE(h.model.count(), 2);

        // Through the client's signal, as the backend emits it.
        Q_EMIT h.client.eventRedacted(kRoom, QStringLiteral("$del1"));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("reaction"));
        // A redaction in another room with the same id is not this one.
        Q_EMIT h.client.eventRedacted(QStringLiteral("!other:mock.local"),
                                      QStringLiteral("$re1"));
        QCOMPARE(h.model.count(), 1);
        // The reaction, by its own id.
        Q_EMIT h.client.eventRedacted(kRoom, QStringLiteral("$re1"));
        QCOMPARE(h.model.count(), 0);
        QCOMPARE(h.model.unseenCount(), 0);
        // A late copy of the deleted message is refused.
        QVERIFY(!h.model.ingest(m, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 0);
    }

    // Finding 2. A left, kicked or banned room drops out of the room list;
    // its rows used to stay unseen for the session.
    void aLeftRoomsRowsAreDropped()
    {
        Harness h;
        RoomInfo room;
        room.id = kRoom;
        room.name = QStringLiteral("Lounge");
        room.highlightCount = 1;
        room.hasUnreadMessages = true;
        h.client.roomSet = { room };
        TimelineEvent m = text(QStringLiteral("$lf1"), QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me"), 1000);
        m.mentionsMe = true;
        QVERIFY(h.model.ingest(m, QStringLiteral("Lounge")));
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.count(), 1);

        // Gone from the list. A reset can do that for a moment; that is not
        // a leave.
        h.client.roomSet = {};
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.count(), 1);
        h.advance(ActivityModel::kLeftRoomGraceMs - 1000);
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.count(), 1);
        // Back within the grace: nothing happened.
        h.client.roomSet = { room };
        Q_EMIT h.client.roomsChanged();
        h.client.roomSet = {};
        Q_EMIT h.client.roomsChanged();
        h.advance(ActivityModel::kLeftRoomGraceMs - 1000);
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.count(), 1);
        // Gone for longer than the grace: the rows go.
        h.advance(2000);
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.count(), 0);
        QCOMPARE(h.model.unseenCount(), 0);

        // A room the client reports as Left goes at once.
        TimelineEvent m2 = m;
        m2.eventId = QStringLiteral("$lf2");
        QVERIFY(h.model.ingest(m2, QStringLiteral("Lounge")));
        RoomInfo left = room;
        left.membership = RoomInfo::Left;
        h.client.roomSet = { left };
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.count(), 0);
    }

    // Finding 2, seed half: a seeded row from a room this client has never
    // known (left long ago) was deferred for ever and stayed unseen after
    // every restart.
    void aSeededRowFromARoomNeverKnownGoesOnceTheListHasLoaded()
    {
        Harness h;
        h.client.roomSet = {};
        h.model.seed({ QVariantMap{
            { QStringLiteral("eventId"), QStringLiteral("$gone1") },
            { QStringLiteral("roomId"), QStringLiteral("!longgone:mock.local") },
            { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
            { QStringLiteral("timestampMs"), 100 },
            { QStringLiteral("read"), false } } });
        QCOMPARE(h.model.unseenCount(), 1);
        Q_EMIT h.client.roomsChanged();
        h.advance(ActivityModel::kUnknownRoomGraceMs + 1000);
        Q_EMIT h.client.roomsChanged();
        QCOMPARE(h.model.count(), 0);
    }

    // Finding 10. Every row restored after a restart is kind "highlight", and
    // the Mentions tab used to hide all of them.
    void aHighlightRowIsVisibleUnderMentions()
    {
        Harness h;
        h.model.seed({ QVariantMap{
            { QStringLiteral("eventId"), QStringLiteral("$hl1") },
            { QStringLiteral("roomId"), kRoom },
            { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
            { QStringLiteral("timestampMs"), 100 },
            { QStringLiteral("read"), false } } });
        h.model.setFilter(QStringLiteral("mentions"));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("highlight"));
    }

    // Finding 11. The room's one unread highlight is already listed from the
    // initial-sync window; the seed skips its id. Old code still spent the
    // budget of 1 on a seeded row, so the bell said 2 and the room list 1.
    void theSeedBudgetCountsARowAlreadyListedLive()
    {
        Harness h;
        RoomInfo room;
        room.id = kRoom;
        room.name = QStringLiteral("Lounge");
        room.highlightCount = 1;
        room.hasUnreadMessages = true;
        h.client.roomSet = { room };
        TimelineEvent live = text(QStringLiteral("$E"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("hey @me"), 500);
        live.mentionsMe = true;
        QVERIFY(h.model.ingest(live, QStringLiteral("Lounge")));
        QVariantList rows;
        for (const auto &[id, ts] : { std::pair{ "$E", 500 }, std::pair{ "$D", 400 },
                                      std::pair{ "$C", 300 } }) {
            rows.append(QVariantMap{
                { QStringLiteral("eventId"), QString::fromLatin1(id) },
                { QStringLiteral("roomId"), kRoom },
                { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                { QStringLiteral("timestampMs"), ts },
                { QStringLiteral("read"), false } });
        }
        h.model.seed(rows);
        QCOMPARE(h.model.count(), 3);
        QCOMPARE(h.model.unseenCount(), 1);
    }

    // Finding 14. Room modes are per account; the cache keyed by room id
    // survived a switch and hid the next account's rows in a shared room.
    void theMuteCacheDoesNotOutliveTheAccount()
    {
        Harness h;
        int mode = 2;   // account A muted the room
        h.model.setNotificationModeSource([&mode](const QString &) { return mode; });
        TimelineEvent m = text(QStringLiteral("$mu1"), QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me"), 1000);
        m.mentionsMe = true;
        QVERIFY(h.model.ingest(m, QStringLiteral("Lounge")));
        QCOMPARE(h.model.unseenCount(), 0);       // cached as muted

        Q_EMIT h.client.loggedOut();              // switch to account B
        mode = 0;                                 // B has the room unmuted
        TimelineEvent b = m;
        b.eventId = QStringLiteral("$mu2");
        QVERIFY(h.model.ingest(b, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.model.unseenCount(), 1);
    }

    // Finding 131. The open thread's copy carries the composite timeline id;
    // old code stored it as the row's room, so no read path matched and the
    // composite reached QML through RoomIdRole.
    void aThreadCopysCompositeIdIsReducedToTheRoom()
    {
        Harness h;
        TimelineEvent m = text(QStringLiteral("$cp1"), QStringLiteral("@bob:mock.local"),
                               QStringLiteral("hey @me"), 1000);
        m.mentionsMe = true;
        m.roomId = MatrixClient::threadTimelineId(kRoom, QStringLiteral("$root"));
        QVERIFY(h.model.ingest(m, QStringLiteral("Lounge")));
        QCOMPARE(h.row(0).value(QStringLiteral("roomId")).toString(), kRoom);
        QCOMPARE(h.row(0).value(QStringLiteral("threadRootId")).toString(),
                 QStringLiteral("$root"));
        QVERIFY(!MatrixClient::isThreadTimelineId(
            h.row(0).value(QStringLiteral("roomId")).toString()));
        // The thread's receipt, named by the real room, clears it.
        h.model.markRoomReadUpTo(kRoom, 1000, QStringLiteral("$root"));
        QCOMPARE(h.model.unseenCount(), 0);
    }

    // Findings 5/15/154. With a push verdict the rules decide: a highlight
    // without a direct mention lists (old code ignored it), a raw
    // m.mentions.room the rules did not highlight does not (old code listed
    // it for any member), and the bell's local keyword list no longer
    // classifies over the rules.
    void thePushVerdictDecidesHighlights()
    {
        Harness h;
        h.model.setKeywords({ QStringLiteral("deploy") });
        TimelineEvent kw = text(QStringLiteral("$pv1"), QStringLiteral("@bob:mock.local"),
                                QStringLiteral("server keyword hit"), 1000);
        kw.pushVerdict = TimelineEvent::PushVerdict::Notify;
        kw.pushHighlight = true;
        QVERIFY(h.model.ingest(kw, QStringLiteral("Ops")));
        QCOMPARE(h.kindOf(QStringLiteral("$pv1")), QStringLiteral("highlight"));

        TimelineEvent room = text(QStringLiteral("$pv2"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("@room everyone"), 1100);
        room.mentionsRoom = true;
        room.pushVerdict = TimelineEvent::PushVerdict::Notify;   // no power
        QVERIFY(!h.model.ingest(room, QStringLiteral("Ops")));
        room.eventId = QStringLiteral("$pv3");
        room.pushHighlight = true;                               // allowed
        QVERIFY(h.model.ingest(room, QStringLiteral("Ops")));
        QCOMPARE(h.kindOf(QStringLiteral("$pv3")), QStringLiteral("room_mention"));

        TimelineEvent local = text(QStringLiteral("$pv4"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("deploy at noon"), 1200);
        local.pushVerdict = TimelineEvent::PushVerdict::Quiet;
        QVERIFY(!h.model.ingest(local, QStringLiteral("Ops")));
        // Without a verdict (no rule evaluation) the legacy list still works.
        local.eventId = QStringLiteral("$pv5");
        local.pushVerdict = TimelineEvent::PushVerdict::Unknown;
        QVERIFY(h.model.ingest(local, QStringLiteral("Ops")));
        QCOMPARE(h.kindOf(QStringLiteral("$pv5")), QStringLiteral("keyword"));
    }

    // Finding 8. Element puts the replied-to user in m.mentions; old code
    // checked mentionsMe first and every reply landed under Mentions.
    void aReplyThatAlsoMentionsMeIsAReply()
    {
        Harness h;
        QVERIFY(!h.model.ingest(text(QStringLiteral("$mine"), h.client.self,
                                     QStringLiteral("question?"), 1000),
                                QStringLiteral("Lounge")));
        TimelineEvent reply = text(QStringLiteral("$rm1"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("answer"), 2000);
        reply.replyToEventId = QStringLiteral("$mine");
        reply.mentionsMe = true;
        QVERIFY(h.model.ingest(reply, QStringLiteral("Lounge")));
        QCOMPARE(h.kindOf(QStringLiteral("$rm1")), QStringLiteral("reply"));
        // A mention in a reply to someone else stays a mention.
        TimelineEvent other = text(QStringLiteral("$rm2"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("cc @me"), 2100);
        other.replyToEventId = QStringLiteral("$carols");
        other.replyToSenderId = QStringLiteral("@carol:mock.local");
        other.mentionsMe = true;
        QVERIFY(h.model.ingest(other, QStringLiteral("Lounge")));
        QCOMPARE(h.kindOf(QStringLiteral("$rm2")), QStringLiteral("mention"));
    }

    // Finding 9. Each edit has a fresh id; old code listed every edit as a new
    // row with the "* " fallback as its text.
    void anEditUpdatesItsRowAndNeverAddsOne()
    {
        Harness h;
        h.model.setKeywords({ QStringLiteral("deploy") });
        TimelineEvent orig = text(QStringLiteral("$o1"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("hey @me deplyo"), 1000);
        orig.mentionsMe = true;
        QVERIFY(h.model.ingest(orig, QStringLiteral("Ops")));
        TimelineEvent edit = text(QStringLiteral("$e1"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("* hey @me deploy"), 1100);
        edit.isEdit = true;
        edit.replacesEventId = QStringLiteral("$o1");
        edit.editNewBody = QStringLiteral("hey @me deploy");
        QVERIFY(!h.model.ingest(edit, QStringLiteral("Ops")));
        TimelineEvent edit2 = edit;
        edit2.eventId = QStringLiteral("$e2");
        edit2.editNewBody = QStringLiteral("hey @me, deploy now");
        QVERIFY(!h.model.ingest(edit2, QStringLiteral("Ops")));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("hey @me, deploy now"));

        // An edit of a message never listed lists only for a NEW mention, and
        // once, as the original message.
        TimelineEvent quiet = text(QStringLiteral("$e3"), QStringLiteral("@bob:mock.local"),
                                   QStringLiteral("* deploy"), 1200);
        quiet.isEdit = true;
        quiet.replacesEventId = QStringLiteral("$o2");
        quiet.editNewBody = QStringLiteral("deploy");
        QVERIFY(!h.model.ingest(quiet, QStringLiteral("Ops")));
        TimelineEvent newly = quiet;
        newly.eventId = QStringLiteral("$e4");
        newly.mentionsMe = true;
        newly.editNewBody = QStringLiteral("deploy, @me");
        QVERIFY(h.model.ingest(newly, QStringLiteral("Ops")));
        newly.eventId = QStringLiteral("$e5");
        QVERIFY(!h.model.ingest(newly, QStringLiteral("Ops")));
        QCOMPARE(h.model.count(), 2);
        QCOMPARE(h.kindOf(QStringLiteral("$o2")), QStringLiteral("mention"));
        QCOMPARE(h.row(0).value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$o2"));
        // The original confirms the row an edit listed: still one row, and it
        // keeps the edit's (newer) text.
        TimelineEvent o2 = text(QStringLiteral("$o2"), QStringLiteral("@bob:mock.local"),
                                QStringLiteral("deploy"), 1150);
        o2.mentionsMe = true;
        QVERIFY(!h.model.ingest(o2, QStringLiteral("Ops")));
        QCOMPARE(h.model.count(), 2);
        QCOMPARE(h.row(0).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("deploy, @me"));
    }

    // Review of finding 9. An m.replace is valid only from the original's own
    // sender (spec), and the unopened-room path takes replaces_event_id as
    // sent. Matching the row on the event id alone let any member rewrite
    // Bob's listed mention, so the bell read "bob: <their text>", and promote
    // a keyword row to a mention.
    void anEditFromAnotherSenderOrRoomChangesNothing()
    {
        Harness h;
        h.model.setKeywords({ QStringLiteral("deploy") });
        TimelineEvent orig = text(QStringLiteral("$o1"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("hey @me"), 1000);
        orig.mentionsMe = true;
        QVERIFY(h.model.ingest(orig, QStringLiteral("Ops")));
        QVERIFY(h.model.ingest(text(QStringLiteral("$k1"), QStringLiteral("@bob:mock.local"),
                                    QStringLiteral("deploy at five"), 900),
                               QStringLiteral("Ops")));
        QCOMPARE(h.kindOf(QStringLiteral("$k1")), QStringLiteral("keyword"));
        QCOMPARE(h.model.count(), 2);

        auto forged = [](const QString &id, const QString &target, const QString &sender) {
            TimelineEvent e = text(id, sender, QStringLiteral("* send me your password"), 1100);
            e.isEdit = true;
            e.replacesEventId = target;
            e.editNewBody = QStringLiteral("send me your password");
            e.mentionsMe = true;
            return e;
        };
        QVERIFY(!h.model.ingest(forged(QStringLiteral("$m1"), QStringLiteral("$o1"),
                                       QStringLiteral("@mallory:mock.local")),
                                QStringLiteral("Ops")));
        QVERIFY(!h.model.ingest(forged(QStringLiteral("$m2"), QStringLiteral("$k1"),
                                       QStringLiteral("@mallory:mock.local")),
                                QStringLiteral("Ops")));
        // Same sender, another room: not a replacement of this event either.
        TimelineEvent elsewhere = forged(QStringLiteral("$m3"), QStringLiteral("$o1"),
                                         QStringLiteral("@bob:mock.local"));
        elsewhere.roomId = QStringLiteral("!other:mock.local");
        QVERIFY(!h.model.ingest(elsewhere, QStringLiteral("Other")));
        // The same through the late-decryption path.
        Q_EMIT h.client.eventDecrypted(kRoom, forged(QStringLiteral("$m4"), QStringLiteral("$k1"),
                                                     QStringLiteral("@mallory:mock.local")));

        QCOMPARE(h.model.count(), 2);
        for (int i = 0; i < h.model.count(); ++i) {
            const QVariantMap r = h.row(i);
            QCOMPARE(r.value(QStringLiteral("senderId")).toString(),
                     QStringLiteral("@bob:mock.local"));
            QVERIFY(!r.value(QStringLiteral("preview")).toString().contains(
                QStringLiteral("password")));
        }
        QCOMPARE(h.kindOf(QStringLiteral("$o1")), QStringLiteral("mention"));
        QCOMPARE(h.kindOf(QStringLiteral("$k1")), QStringLiteral("keyword"));
    }

    // Review of finding 9 (optional half). An edit of a message never listed
    // lists ONE row under the original's id, before the original is seen. A
    // forged edit holding that id must not keep the real original out.
    void aForgedEditCannotHoldTheOriginalsRow()
    {
        Harness h;
        auto forged = [](const QString &id, const QString &target) {
            TimelineEvent e = text(id, QStringLiteral("@mallory:mock.local"),
                                   QStringLiteral("* fake"), 1100);
            e.isEdit = true;
            e.replacesEventId = target;
            e.editNewBody = QStringLiteral("fake");
            e.mentionsMe = true;
            return e;
        };
        // Live original.
        QVERIFY(h.model.ingest(forged(QStringLiteral("$m1"), QStringLiteral("$r1")),
                               QStringLiteral("Ops")));
        TimelineEvent real = text(QStringLiteral("$r1"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("hey @me, real"), 1000);
        real.mentionsMe = true;
        QVERIFY(h.model.ingest(real, QStringLiteral("Ops")));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("senderId")).toString(),
                 QStringLiteral("@bob:mock.local"));
        QCOMPARE(h.row(0).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("hey @me, real"));

        // Late-decrypted original.
        QVERIFY(h.model.ingest(forged(QStringLiteral("$m2"), QStringLiteral("$r2")),
                               QStringLiteral("Ops")));
        TimelineEvent dec = text(QStringLiteral("$r2"), QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("decrypted @me"), 1200);
        dec.mentionsMe = true;
        Q_EMIT h.client.eventDecrypted(kRoom, dec);
        QCOMPARE(h.model.count(), 2);
        for (int i = 0; i < h.model.count(); ++i)
            QCOMPARE(h.row(i).value(QStringLiteral("senderId")).toString(),
                     QStringLiteral("@bob:mock.local"));

        // Seeded original.
        QVERIFY(h.model.ingest(forged(QStringLiteral("$m3"), QStringLiteral("$r3")),
                               QStringLiteral("Ops")));
        QCOMPARE(h.model.count(), 3);
        h.model.seed({ QVariantMap{
            { QStringLiteral("eventId"), QStringLiteral("$r3") },
            { QStringLiteral("roomId"), kRoom },
            { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
            { QStringLiteral("senderName"), QStringLiteral("bob") },
            { QStringLiteral("kind"), QStringLiteral("mention") },
            { QStringLiteral("preview"), QStringLiteral("seeded @me") },
            { QStringLiteral("timestampMs"), 1300 },
        } });
        QCOMPARE(h.model.count(), 3);
        for (int i = 0; i < h.model.count(); ++i)
            QCOMPARE(h.row(i).value(QStringLiteral("senderId")).toString(),
                     QStringLiteral("@bob:mock.local"));
    }

    // Finding 6. A mention that decrypts late was never classified: the first
    // ingest saw no body and no m.mentions, and nothing re-ran it.
    void aLateDecryptedMentionIsListedOnce()
    {
        Harness h;
        TimelineEvent enc = text(QStringLiteral("$ld1"), QStringLiteral("@bob:mock.local"),
                                 QString(), 1000);
        enc.undecryptable = true;
        QVERIFY(!h.model.ingest(enc, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 0);

        TimelineEvent dec = enc;
        dec.undecryptable = false;
        dec.body = QStringLiteral("hey @me, the secret");
        dec.mentionsMe = true;
        Q_EMIT h.client.eventDecrypted(kRoom, dec);
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("mention"));
        QCOMPARE(h.row(0).value(QStringLiteral("encrypted")).toBool(), false);
        // Again (a second producer): still one row, same id.
        Q_EMIT h.client.eventDecrypted(kRoom, dec);
        QCOMPARE(h.model.count(), 1);

        // An already-listed undecryptable row (its thread relation is in the
        // clear) is replaced in place, keeping its seen state.
        QVERIFY(!h.model.ingest(text(QStringLiteral("$myroot"), h.client.self,
                                     QStringLiteral("root"), 900),
                                QStringLiteral("Lounge")));
        TimelineEvent t = enc;
        t.eventId = QStringLiteral("$ld2");
        t.threadRootId = QStringLiteral("$myroot");
        QVERIFY(h.model.ingest(t, QStringLiteral("Lounge")));
        h.model.markSeen(QStringLiteral("$ld2"));
        TimelineEvent td = t;
        td.undecryptable = false;
        td.body = QStringLiteral("now readable");
        Q_EMIT h.client.eventDecrypted(kRoom, td);
        QCOMPARE(h.model.count(), 2);
        QCOMPARE(h.kindOf(QStringLiteral("$ld2")), QStringLiteral("thread"));
        QCOMPARE(h.row(0).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("now readable"));
        QCOMPARE(h.row(0).value(QStringLiteral("seen")).toBool(), true);
    }

    // Finding 12. Stripped invite state has no timestamp; old code stamped
    // "now" on every launch, so a seen two-week-old invite came back as the
    // newest unseen row after each restart.
    void aPendingInviteKeepsItsTimeAndSeenStateAcrossARestart()
    {
        Harness h;
        RoomInfo r;
        r.id = QStringLiteral("!inv:mock.local");
        r.name = QStringLiteral("Secret club");
        r.membership = RoomInfo::Invited;
        r.inviterUserId = QStringLiteral("@bob:mock.local");
        h.advance(-14LL * 24 * 3600 * 1000);   // noted two weeks ago
        QVERIFY(h.model.noteInvite(r));
        const qint64 firstNoted = h.row(0).value(QStringLiteral("timestampMs")).toLongLong();
        h.model.markSeen(QStringLiteral("invite:") + r.id);
        QCOMPARE(h.model.unseenCount(), 0);
        const QVariantMap invites = h.stored.value(QStringLiteral("invites")).toMap();
        QVERIFY(invites.contains(r.id));
        // Only the room id persists: nothing a user typed or a name.
        QVERIFY(!QVariant(invites).toString().contains(QStringLiteral("Secret")));

        // A restart: a fresh model on the same store.
        QVariantMap stored = h.stored;
        FakeClient client;
        ActivityModel next;
        next.setStore({ [&] { return stored; }, [&](const QVariantMap &m) { stored = m; } });
        next.setClient(&client);
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
        QVERIFY(next.noteInvite(r));
        QCOMPARE(next.entryAt(0).value(QStringLiteral("timestampMs")).toLongLong(),
                 firstNoted);
        QCOMPARE(next.unseenCount(), 0);
        // Answered: forgotten.
        next.inviteResolved(r.id);
        QVERIFY(!stored.value(QStringLiteral("invites")).toMap().contains(r.id));
    }

    // Finding 13. The store loaded only on the first Syncing, which an offline
    // restore never reaches; a keyword or marker written before then replaced
    // the account's real state, and clear() kept the last account's keywords.
    void theStoreLoadsWithTheAccountAndNothingIsSavedBeforeIt()
    {
        QVariantMap stored{ { QStringLiteral("seenUpToMs"), 5000 },
                            { QStringLiteral("keywords"), QStringList{ QStringLiteral("ops") } } };
        int saves = 0;
        FakeClient client;
        client.loggedIn = false;
        ActivityModel model;
        model.setStore({ [&] { return stored; },
                         [&](const QVariantMap &m) { stored = m; ++saves; } });
        model.setClient(&client);
        QCoreApplication::processEvents();
        QVERIFY(model.keywords().isEmpty());
        // Before the load nothing is written over the account's state.
        model.setKeywords({ QStringLiteral("other") });
        QCOMPARE(saves, 0);
        QCOMPARE(stored.value(QStringLiteral("keywords")).toStringList(),
                 QStringList{ QStringLiteral("ops") });
        // Signed in, never Syncing (an offline restore): the store loads.
        client.loggedIn = true;
        Q_EMIT client.loginSucceeded(client.self);
        QTRY_COMPARE(model.keywords(), QStringList{ QStringLiteral("ops") });
        // Sign-out forgets the keywords with everything else.
        Q_EMIT client.loggedOut();
        QVERIFY(model.keywords().isEmpty());
    }

    // Finding 16. An ignored user's in-flight mention, reaction or invite is
    // not activity, and ignoring someone drops what they already have listed.
    void anIgnoredUserIsNotActivity()
    {
        Harness h;
        QSet<QString> ignored;
        h.model.setIgnoredCheck([&ignored](const QString &id) { return ignored.contains(id); });
        QVERIFY(!h.model.ingest(text(QStringLiteral("$mine"), h.client.self,
                                     QStringLiteral("joke"), 900),
                                QStringLiteral("Lounge")));
        TimelineEvent m = text(QStringLiteral("$ig1"), QStringLiteral("@troll:mock.local"),
                               QStringLiteral("@me hello"), 1000);
        m.mentionsMe = true;
        QVERIFY(h.model.ingest(m, QStringLiteral("Lounge")));
        QVERIFY(h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                     QStringLiteral("$igr"), QStringLiteral("$mine"),
                                     QStringLiteral("@troll:mock.local"), QString(),
                                     QStringLiteral("💩"), 1100));
        TimelineEvent keep = text(QStringLiteral("$ok1"), QStringLiteral("@bob:mock.local"),
                                  QStringLiteral("@me hi"), 1200);
        keep.mentionsMe = true;
        QVERIFY(h.model.ingest(keep, QStringLiteral("Lounge")));
        QCOMPARE(h.model.count(), 3);

        ignored.insert(QStringLiteral("@troll:mock.local"));
        h.model.dropSender(QStringLiteral("@troll:mock.local"));
        QCOMPARE(h.model.count(), 1);
        QCOMPARE(h.row(0).value(QStringLiteral("entryId")).toString(),
                 QStringLiteral("$ok1"));
        TimelineEvent again = m;
        again.eventId = QStringLiteral("$ig2");
        QVERIFY(!h.model.ingest(again, QStringLiteral("Lounge")));
        QVERIFY(!h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                      QStringLiteral("$igr2"), QStringLiteral("$mine"),
                                      QStringLiteral("@troll:mock.local"), QString(),
                                      QStringLiteral("💩"), 1300));
        RoomInfo inv;
        inv.id = QStringLiteral("!trap:mock.local");
        inv.membership = RoomInfo::Invited;
        inv.inviterUserId = QStringLiteral("@troll:mock.local");
        QVERIFY(!h.model.noteInvite(inv));
        QCOMPARE(h.model.count(), 1);
    }

    // Finding 17. A seeded preview was the raw body: a reply's fallback
    // quote, a media message's filename.
    void aSeededPreviewReadsLikeALiveOne()
    {
        Harness h;
        h.model.seed({
            QVariantMap{ { QStringLiteral("eventId"), QStringLiteral("$sp1") },
                         { QStringLiteral("roomId"), kRoom },
                         { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                         { QStringLiteral("preview"),
                           QStringLiteral("> <@alice:mock.local> what alice said\n\n"
                                          "bob's actual reply @me") },
                         { QStringLiteral("msgtype"), QStringLiteral("m.text") },
                         { QStringLiteral("timestampMs"), 200 } },
            QVariantMap{ { QStringLiteral("eventId"), QStringLiteral("$sp2") },
                         { QStringLiteral("roomId"), kRoom },
                         { QStringLiteral("senderId"), QStringLiteral("@bob:mock.local") },
                         { QStringLiteral("preview"), QStringLiteral("IMG_2041.jpg") },
                         { QStringLiteral("msgtype"), QStringLiteral("m.image") },
                         { QStringLiteral("timestampMs"), 100 } },
        });
        QCOMPARE(h.row(0).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("bob's actual reply @me"));
        QCOMPARE(h.row(1).value(QStringLiteral("preview")).toString(),
                 QStringLiteral("📷"));
    }

    // Finding 156. The own-event memory is this session's only; a reaction to
    // a message sent before this launch (or from a phone) was dropped. The
    // backend's confirmation now counts.
    void aReactionTheBackendConfirmsAsOwnIsListed()
    {
        Harness h;
        QVERIFY(!h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                      QStringLiteral("$x1"), QStringLiteral("$yesterday"),
                                      QStringLiteral("@bob:mock.local"),
                                      QStringLiteral("bob"), QStringLiteral("👍"), 2000));
        QVERIFY(h.model.noteReaction(kRoom, QStringLiteral("Lounge"),
                                     QStringLiteral("$x2"), QStringLiteral("$yesterday"),
                                     QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("bob"), QStringLiteral("👍"), 2000,
                                     /*targetIsOwn*/ true));
        QCOMPARE(h.model.count(), 1);
    }
};

QTEST_GUILESS_MAIN(ActivityModelTest)
#include "ActivityModelTest.moc"
