// v0.7 outgoing @-mentions: the current-room member suggestion model. Requests
// members on activation, filters/ranks/dedups against the typed query, excludes
// the signed-in user, rejects stale op ids, clears on room switch + logout, and
// re-requests on an authoritative membership change. No network, no homeserver.

#include "matrix/MockMatrixClient.h"
#include "models/MentionSuggestionModel.h"

#include <QtTest/QtTest>

namespace {

// A mock whose member requests are fully test-driven: the test controls the
// op id, the delivered snapshot, and the membership-change / logout signals.
class MemberMock : public MockMatrixClient
{
    Q_OBJECT
public:
    quint64 requestRoomMembers(const QString &roomId) override
    {
        ++m_requestCount;
        m_lastRoomId = roomId;
        return ++m_op;
    }

    void deliver(quint64 op, const QString &roomId,
                 const QVariantList &members, bool ok = true)
    {
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("ok"), ok);
        snapshot.insert(QStringLiteral("members"), members);
        Q_EMIT roomMembersReceived(op, roomId, snapshot);
    }
    // A snapshot that positively states the @room permission, as the Rust
    // backend's roster does. `deliver` above omits the key entirely, which is
    // the honest shape for a backend that does not report it.
    void deliverWithPermission(quint64 op, const QString &roomId,
                               bool canNotifyRoom)
    {
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("ok"), true);
        snapshot.insert(QStringLiteral("members"), QVariantList{});
        snapshot.insert(QStringLiteral("canNotifyRoom"), canNotifyRoom);
        Q_EMIT roomMembersReceived(op, roomId, snapshot);
    }
    void emitMembersChanged(const QString &roomId)
    {
        // The model re-requests on the sync poke since review H1;
        // membersChanged now only drives presentation consumers.
        Q_EMIT roomMemberEventSeen(roomId);
    }
    void emitLoggedOut() { Q_EMIT loggedOut(); }

    quint64 m_op = 0;
    int m_requestCount = 0;
    QString m_lastRoomId;
};

QVariantMap member(const QString &userId, const QString &name,
                   bool isOwn = false, bool ambiguous = false,
                   const QString &membership = QStringLiteral("join"))
{
    QVariantMap m;
    m.insert(QStringLiteral("userId"), userId);
    m.insert(QStringLiteral("displayName"), name);
    m.insert(QStringLiteral("avatarUrl"), QString());
    m.insert(QStringLiteral("membership"), membership);
    m.insert(QStringLiteral("ambiguous"), ambiguous);
    m.insert(QStringLiteral("isOwn"), isOwn);
    return m;
}

} // namespace

class MentionSuggestionModelTest : public QObject
{
    Q_OBJECT

private slots:
    void matchScorePolicy()
    {
        // Empty query matches everything; a missing token excludes.
        QVERIFY(MentionSuggestionModel::matchScore(
                    QString(), QStringLiteral("Alice"), QStringLiteral("@a:hs"))
                >= 0);
        QVERIFY(MentionSuggestionModel::matchScore(
                    QStringLiteral("zzz"), QStringLiteral("Alice"),
                    QStringLiteral("@a:hs"))
                < 0);
        // Prefix beats bare substring on the same field.
        QVERIFY(MentionSuggestionModel::matchScore(
                    QStringLiteral("al"), QStringLiteral("Alice"),
                    QStringLiteral("@alice:hs"))
                > MentionSuggestionModel::matchScore(
                    QStringLiteral("ice"), QStringLiteral("Alice"),
                    QStringLiteral("@alice:hs")));
        // A localpart-only match still counts.
        QVERIFY(MentionSuggestionModel::matchScore(
                    QStringLiteral("alice"), QStringLiteral("Zoe"),
                    QStringLiteral("@alice:hs"))
                >= 0);
    }

    void requestsOnActivationAndFiltersRanks()
    {
        MemberMock mock;
        MentionSuggestionModel model;
        // Member filtering only. The whole-room row is offered by default and
        // is not a member, so it is turned off here; it has its own coverage
        // in MentionPopupContractTest.
        model.setClient(&mock);

        model.setRoomId(QStringLiteral("!r:hs"));
        model.setRoomMentionAllowed(false);
        QCOMPARE(mock.m_requestCount, 1);
        QCOMPARE(mock.m_lastRoomId, QStringLiteral("!r:hs"));

        mock.deliver(mock.m_op, QStringLiteral("!r:hs"),
                     { member(QStringLiteral("@alice:hs"),
                              QStringLiteral("Alice")),
                       member(QStringLiteral("@bob:hs"), QStringLiteral("Bob")),
                       member(QStringLiteral("@carol:hs"),
                              QStringLiteral("Carol Bobson")) });
        QCOMPARE(model.count(), 3); // empty query lists everyone

        model.setQuery(QStringLiteral("bob"));
        QCOMPARE(model.count(), 2); // Alice excluded
        // A display-name prefix (Bob) ranks above a word-start (Carol Bobson).
        QCOMPARE(model.get(0).value(QStringLiteral("userId")).toString(),
                 QStringLiteral("@bob:hs"));
    }

    void excludesOwnUser()
    {
        MemberMock mock;
        MentionSuggestionModel model;
        // Member filtering only. The whole-room row is offered by default and
        // is not a member, so it is turned off here; it has its own coverage
        // in MentionPopupContractTest.
        model.setClient(&mock);
        model.setRoomId(QStringLiteral("!r:hs"));
        model.setRoomMentionAllowed(false);
        mock.deliver(mock.m_op, QStringLiteral("!r:hs"),
                     { member(QStringLiteral("@me:hs"), QStringLiteral("Me"),
                              /*isOwn=*/true),
                       member(QStringLiteral("@bob:hs"),
                              QStringLiteral("Bob")) });
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.get(0).value(QStringLiteral("userId")).toString(),
                 QStringLiteral("@bob:hs"));
    }

    // 2026-08-14 (unban round): the snapshot now carries banned members
    // and the filter is an ALLOW-list — only joined/invited (either
    // spelling) are suggestable; a banned row and any unknown label fail
    // closed instead of being suggested.
    void excludesBannedAndUnknownMemberships()
    {
        MemberMock mock;
        MentionSuggestionModel model;
        // Member filtering only. The whole-room row is offered by default and
        // is not a member, so it is turned off here; it has its own coverage
        // in MentionPopupContractTest.
        model.setClient(&mock);
        model.setRoomId(QStringLiteral("!r:hs"));
        model.setRoomMentionAllowed(false);
        mock.deliver(mock.m_op, QStringLiteral("!r:hs"),
                     { member(QStringLiteral("@bob:hs"),
                              QStringLiteral("Bob")),
                       member(QStringLiteral("@inv:hs"),
                              QStringLiteral("Invitee"),
                              /*isOwn=*/false, /*ambiguous=*/false,
                              QStringLiteral("invited")),
                       member(QStringLiteral("@troll:hs"),
                              QStringLiteral("Troll"),
                              /*isOwn=*/false, /*ambiguous=*/false,
                              QStringLiteral("banned")),
                       member(QStringLiteral("@odd:hs"),
                              QStringLiteral("Odd"),
                              /*isOwn=*/false, /*ambiguous=*/false,
                              QStringLiteral("knock")) });
        QCOMPARE(model.count(), 2);
        QStringList ids;
        for (int i = 0; i < model.count(); ++i)
            ids.append(model.get(i).value(QStringLiteral("userId")).toString());
        std::sort(ids.begin(), ids.end());
        QCOMPARE(ids, (QStringList{ QStringLiteral("@bob:hs"),
                                    QStringLiteral("@inv:hs") }));
    }

    void dedupsByMxid()
    {
        MemberMock mock;
        MentionSuggestionModel model;
        // Member filtering only. The whole-room row is offered by default and
        // is not a member, so it is turned off here; it has its own coverage
        // in MentionPopupContractTest.
        model.setClient(&mock);
        model.setRoomId(QStringLiteral("!r:hs"));
        model.setRoomMentionAllowed(false);
        mock.deliver(mock.m_op, QStringLiteral("!r:hs"),
                     { member(QStringLiteral("@bob:hs"), QStringLiteral("Bob")),
                       member(QStringLiteral("@bob:hs"),
                              QStringLiteral("Bob")) });
        QCOMPARE(model.count(), 1);
    }

    void rejectsStaleOpId()
    {
        MemberMock mock;
        MentionSuggestionModel model;
        // Member filtering only. The whole-room row is offered by default and
        // is not a member, so it is turned off here; it has its own coverage
        // in MentionPopupContractTest.
        model.setClient(&mock);

        model.setRoomId(QStringLiteral("!r1:hs"));
        model.setRoomMentionAllowed(false);
        const quint64 op1 = mock.m_op;
        model.setRoomId(QStringLiteral("!r2:hs")); // supersedes r1
        // Re-asserted after every switch: a room change puts the @room
        // permission back to UNKNOWN (offered) on purpose, so that one room's
        // "no" cannot follow the user into the next.
        model.setRoomMentionAllowed(false);
        const quint64 op2 = mock.m_op;

        // The old room's answer is stale (op + room mismatch): ignored.
        mock.deliver(op1, QStringLiteral("!r1:hs"),
                     { member(QStringLiteral("@x:hs"), QStringLiteral("X")) });
        QCOMPARE(model.count(), 0);

        // The current request's answer is applied.
        mock.deliver(op2, QStringLiteral("!r2:hs"),
                     { member(QStringLiteral("@y:hs"), QStringLiteral("Y")) });
        QCOMPARE(model.count(), 1);
    }

    void clearsOnRoomSwitchAndLogout()
    {
        MemberMock mock;
        MentionSuggestionModel model;
        // Member filtering only. The whole-room row is offered by default and
        // is not a member, so it is turned off here; it has its own coverage
        // in MentionPopupContractTest.
        model.setClient(&mock);

        model.setRoomId(QStringLiteral("!r:hs"));
        model.setRoomMentionAllowed(false);
        mock.deliver(mock.m_op, QStringLiteral("!r:hs"),
                     { member(QStringLiteral("@a:hs"), QStringLiteral("A")) });
        QCOMPARE(model.count(), 1);

        model.setRoomId(QStringLiteral("!other:hs")); // switch clears cache
        // See rejectsStaleOpId: the switch also resets the @room permission.
        model.setRoomMentionAllowed(false);
        QCOMPARE(model.count(), 0);

        mock.deliver(mock.m_op, QStringLiteral("!other:hs"),
                     { member(QStringLiteral("@b:hs"), QStringLiteral("B")) });
        QCOMPARE(model.count(), 1);

        mock.emitLoggedOut();
        QCOMPARE(model.count(), 0);
        QVERIFY(model.roomId().isEmpty());
    }

    void reRequestsOnMembersChanged()
    {
        MemberMock mock;
        MentionSuggestionModel model;
        // Member filtering only. The whole-room row is offered by default and
        // is not a member, so it is turned off here; it has its own coverage
        // in MentionPopupContractTest.
        model.setClient(&mock);

        model.setRoomId(QStringLiteral("!r:hs"));
        model.setRoomMentionAllowed(false);
        mock.deliver(mock.m_op, QStringLiteral("!r:hs"),
                     { member(QStringLiteral("@a:hs"), QStringLiteral("A")) });
        const int before = mock.m_requestCount;

        mock.emitMembersChanged(QStringLiteral("!r:hs"));
        QCOMPARE(mock.m_requestCount, before + 1);

        // A change for a different room does not re-request.
        mock.emitMembersChanged(QStringLiteral("!elsewhere:hs"));
        QCOMPARE(mock.m_requestCount, before + 1);
    }

    // The permission still NARROWS — the fix must not be "always offer".
    // A roster snapshot that positively says this account cannot notify the
    // room removes @room; one that says it can keeps it; and one that says
    // NOTHING leaves it offered, because a backend's silence is not a denial.
    void theRosterSnapshotDecidesWhetherRoomIsOffered()
    {
        MemberMock client;
        MentionSuggestionModel model;
        model.setClient(&client);
        model.setRoomId(QStringLiteral("!r:example.org"));
        model.setQuery(QStringLiteral("room"));

        // Silence: offered. This is the mock/non-Rust case and the load
        // window, and it must not read as a refusal.
        client.deliver(client.m_op, QStringLiteral("!r:example.org"), {});
        QVERIFY(model.roomMentionAllowed());
        QCOMPARE(model.rowCount(QModelIndex()), 1);

        // A positive "no" removes it.
        model.setRoomId(QStringLiteral("!s:example.org"));
        model.setQuery(QStringLiteral("room"));
        client.deliverWithPermission(client.m_op,
                                     QStringLiteral("!s:example.org"), false);
        QVERIFY(!model.roomMentionAllowed());
        QCOMPARE(model.rowCount(QModelIndex()), 0);

        // A new room starts UNKNOWN again rather than inheriting that "no".
        model.setRoomId(QStringLiteral("!t:example.org"));
        model.setQuery(QStringLiteral("room"));
        QVERIFY(model.roomMentionAllowed());

        // ...and a positive "yes" keeps it.
        client.deliverWithPermission(client.m_op,
                                     QStringLiteral("!t:example.org"), true);
        QVERIFY(model.roomMentionAllowed());
        QCOMPARE(model.rowCount(QModelIndex()), 1);
    }
};

QTEST_MAIN(MentionSuggestionModelTest)
#include "MentionSuggestionModelTest.moc"
