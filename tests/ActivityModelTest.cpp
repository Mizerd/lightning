// v0.9 (phase 2): the Activity Center model.
//
// Pins the classification and the honesty rules:
//   * mentions, replies to the user's messages, replies in the user's
//     threads, reactions to the user's messages, invites and keyword hits
//     become entries; the user's own messages, local echoes, state rows,
//     redactions and thread-timeline copies never do;
//   * a reaction counts only when its TARGET is the user's message;
//   * keywords match whole words, case-insensitively, never inside a word;
//   * seen state is the model's own: the badge counts unseen entries,
//     "mark all seen" persists a marker and only the marker (previews are
//     never written anywhere), and opening an entry marks it seen and
//     emits the exact navigation triple;
//   * the list is bounded and newest-first; an account switch or sign-out
//     drops everything.
//
// HONEST SCOPE: a fake client emitting the same signals the Rust backend
// emits. A live homeserver, the server-side /notifications seed and the
// panel's rendering are NOT exercised here and are NOT TESTED.

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
        // A reply whose target sender is me, even if the target is older
        // than what this session saw.
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
        // An undecryptable event has no body to match and never lists as a
        // keyword hit on ciphertext.
        TimelineEvent enc = text(QStringLiteral("$k3"), QStringLiteral("@bob:mock.local"),
                                 QStringLiteral("deploy"), 1200);
        enc.undecryptable = true;
        QVERIFY(!h.model.ingest(enc, QStringLiteral("Ops")));
        // ...but a mention that arrived undecryptable still lists, without a
        // preview.
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
        // The store holds the marker and nothing that came from a message.
        QCOMPARE(h.stored.value(QStringLiteral("seenUpToMs")).toLongLong(), 3000);
        for (auto it = h.stored.cbegin(); it != h.stored.cend(); ++it)
            QVERIFY2(!it.value().toString().contains(QStringLiteral("secret")),
                     qPrintable(it.key()));
        QVERIFY(!h.stored.contains(QStringLiteral("entries")));
        // A newer entry is unseen again; an older straggler is covered by
        // the marker.
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
        // The previous account's "own message" memory is gone too: a reply
        // to it under the next account is not "a reply to me".
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
};

QTEST_GUILESS_MAIN(ActivityModelTest)
#include "ActivityModelTest.moc"
