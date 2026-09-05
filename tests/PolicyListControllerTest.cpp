// Mjolnir-style policy lists: the controller's op discipline, and the one
// design decision worth pinning — that following a list does not act on it.
//
// The op rules matter because a policy room can be slow to read (it is a
// whole-room /state fetch over a list that may hold thousands of rules), so
// answers genuinely arrive out of order and for rooms the user has left.

#include "app/PolicyListController.h"
#include "matrix/MockMatrixClient.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

class PolicyClient final : public MockMatrixClient
{
    Q_OBJECT
public:
    bool supportsPolicyLists() const override { return true; }

    int fetchCalls = 0;
    int writeCalls = 0;
    int subscribeCalls = 0;
    QString lastRoom, lastKind, lastEntity, lastRecommendation, lastReason;
    quint64 lastOp = 0;
    bool lastSubscribed = false;

    void fetchPolicyRules(const QString &roomId, quint64 opId) override
    {
        ++fetchCalls;
        lastRoom = roomId;
        lastOp = opId;
    }
    void writePolicyRule(const QString &roomId, const QString &kind,
                         const QString &entity, const QString &recommendation,
                         const QString &reason, quint64 opId) override
    {
        ++writeCalls;
        lastRoom = roomId;
        lastKind = kind;
        lastEntity = entity;
        lastRecommendation = recommendation;
        lastReason = reason;
        lastOp = opId;
    }
    void setPolicySubscribed(const QString &roomId, bool subscribed,
                             quint64 opId) override
    {
        ++subscribeCalls;
        lastRoom = roomId;
        lastSubscribed = subscribed;
        lastOp = opId;
    }
    void fetchPolicySubscriptions(quint64 opId) override { lastOp = opId; }
    int checkCalls = 0;
    void checkPolicyEntity(const QString &kind, const QString &entity,
                           quint64 opId) override
    {
        ++checkCalls;
        lastKind = kind;
        lastEntity = entity;
        lastOp = opId;
    }
};

QVariantList oneRule(const QString &entity = QStringLiteral("@spam:bad.example"))
{
    return QVariantList{ QVariantMap{
        { QStringLiteral("kind"), QStringLiteral("user") },
        { QStringLiteral("entity"), entity },
        { QStringLiteral("recommendation"), QStringLiteral("m.ban") },
        { QStringLiteral("isBan"), true },
        { QStringLiteral("reason"), QStringLiteral("spam") },
        { QStringLiteral("stateKey"), QStringLiteral("rule:") + entity },
    } };
}

const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");

} // namespace

class PolicyListControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void rulesLoadForTheRoomThatWasAskedFor()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        QVERIFY(policy.available());

        policy.openRoom(kRoomA);
        QCOMPARE(client.fetchCalls, 1);
        QVERIFY(policy.loading());
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, oneRule());
        QCOMPARE(policy.rules()->rowCount(), 1);
        QVERIFY(policy.canWrite());
        QVERIFY(!policy.truncated());
        QVERIFY(!policy.loading());
    }

    // A slow read of one room must not land under another room's name. Both
    // guards are exercised, because either alone lets a wrong list through:
    // the op id catches a superseded read of the SAME room, and the room id
    // catches an answer whose op happens to match.
    void anAnswerForAnotherRoomIsNeverShown()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);

        policy.openRoom(kRoomA);
        const quint64 slowOp = client.lastOp;
        policy.openRoom(kRoomB);
        const quint64 fastOp = client.lastOp;
        QVERIFY(slowOp != fastOp);

        // Room A's answer arrives late, after the user moved to room B.
        // Rejected on the OP: it is not the read that is outstanding.
        Q_EMIT client.policyRulesReceived(slowOp, true, kRoomA, true, false,
                                          oneRule());
        QCOMPARE(policy.rules()->rowCount(), 0);
        QCOMPARE(policy.roomId(), kRoomB);

        // Room B's own answer lands.
        Q_EMIT client.policyRulesReceived(fastOp, true, kRoomB, false, false,
                                          oneRule());
        QCOMPARE(policy.rules()->rowCount(), 1);
    }

    // The SECOND guard, on its own: an answer whose op is the outstanding one
    // but whose room is not. It should not happen — the bridge echoes back
    // the room it was asked about — which is exactly why it is checked here
    // rather than trusted. Note the op IS consumed: it is that read's answer,
    // and leaving the slot open would hang `loading` forever.
    void anAnswerNamingTheWrongRoomIsRefusedEvenWhenTheOpMatches()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);

        policy.openRoom(kRoomB);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, oneRule());
        QVERIFY2(policy.rules()->rowCount() == 0,
                 "another room's rules were shown under this room's name");
        QVERIFY2(!policy.canWrite(),
                 "another room's write permission was adopted");
        QVERIFY2(!policy.loading(),
                 "the read must not stay outstanding forever");
    }

    // Opening a second room CLEARS the first room's rules immediately, not
    // when the answer arrives: the gap would show one room's ban list under
    // another room's title.
    void openingARoomClearsThePreviousRulesAtOnce()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, oneRule());
        QCOMPARE(policy.rules()->rowCount(), 1);

        policy.openRoom(kRoomB);
        QCOMPARE(policy.rules()->rowCount(), 0);
        QVERIFY2(!policy.canWrite(),
                 "permission from the previous room must not carry over");
    }

    // A bounded read has to SAY it was bounded, or a partial list reads as a
    // complete one — which for a ban list means "this person is not on it".
    void aTruncatedReadIsReportedAsTruncated()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, false,
                                          true, oneRule());
        QVERIFY(policy.truncated());
    }

    void publishingSendsABanAndRemovingSendsAnEmptyRecommendation()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, {});

        policy.addRule(QStringLiteral("server"),
                       QStringLiteral("  bad.example  "),
                       QStringLiteral("spam"));
        QCOMPARE(client.writeCalls, 1);
        QCOMPARE(client.lastKind, QStringLiteral("server"));
        // Trimmed: a trailing space would make a rule that matches nothing
        // and looks identical in the list.
        QCOMPARE(client.lastEntity, QStringLiteral("bad.example"));
        QCOMPARE(client.lastRecommendation, QStringLiteral("m.ban"));

        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());
        // A success RE-READS rather than applying optimistically, so the list
        // shows what the room holds rather than what we asked for.
        QCOMPARE(client.fetchCalls, 2);

        policy.removeRule(QStringLiteral("server"),
                          QStringLiteral("bad.example"));
        QCOMPARE(client.writeCalls, 2);
        QVERIFY2(client.lastRecommendation.isEmpty(),
                 "removal is an EMPTY recommendation — the bridge writes an "
                 "empty state event, which is the only removal state has");
    }

    // A refused write does not re-read, and says which refusal it was.
    void aRefusedWriteReportsThePermissionCase()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, {});
        const int fetchesBefore = client.fetchCalls;

        QSignalSpy spy(&policy, &PolicyListController::writeFinished);
        policy.addRule(QStringLiteral("user"), QStringLiteral("@a:b.example"),
                       QString());
        Q_EMIT client.policyRuleWritten(client.lastOp, false,
                                        QStringLiteral("forbidden"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(client.fetchCalls, fetchesBefore);
        QVERIFY(policy.lastError().contains(QStringLiteral("permission")));
    }

    // Empty input never reaches the server, and one write at a time.
    void emptyInputIsRefusedAndWritesDoNotRace()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, {});

        policy.addRule(QStringLiteral("user"), QStringLiteral("   "),
                       QString());
        QCOMPARE(client.writeCalls, 0);

        policy.addRule(QStringLiteral("user"), QStringLiteral("@a:b.example"),
                       QString());
        QCOMPARE(client.writeCalls, 1);
        policy.addRule(QStringLiteral("user"), QStringLiteral("@c:b.example"),
                       QString());
        QCOMPARE(client.writeCalls, 1);
    }

    void subscriptionsAreAccountStateAndDoNotSurviveSignOut()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);

        policy.setSubscribed(kRoomA, true);
        QCOMPARE(client.subscribeCalls, 1);
        QVERIFY(client.lastSubscribed);
        Q_EMIT client.policySubscriptionsReceived(client.lastOp, true,
                                                  QString(), { kRoomA });
        QVERIFY(policy.isSubscribed(kRoomA));
        QVERIFY(!policy.isSubscribed(kRoomB));

        // Policy subscriptions are ACCOUNT data. One account's lists must
        // never be shown under the next.
        client.logout();
        QTest::qWait(50);
        QVERIFY(policy.subscriptions().isEmpty());
        QVERIFY(policy.roomId().isEmpty());
        QCOMPARE(policy.rules()->rowCount(), 0);
    }

    // Following a list produces NO enforcement of its own. This is the design
    // decision, and it is here so a later change has to argue with a test
    // rather than slip past: a subscribed list is somebody else's judgement,
    // and silently acting on it is a different feature.
    void followingAListNeverActsOnItsOwn()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        QSignalSpy ignored(&client, &MatrixClient::ignoredUsersChanged);

        policy.setSubscribed(kRoomA, true);
        Q_EMIT client.policySubscriptionsReceived(client.lastOp, true,
                                                  QString(), { kRoomA });
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, false,
                                          false, oneRule());

        QCOMPARE(ignored.count(), 0);
        QCOMPARE(client.writeCalls, 0);
    }

    // The check answers, and a MISS carries no rule detail — a caller must
    // not be able to read a stale reason off a check that found nothing.
    void aCheckThatFoundNothingCarriesNoRuleDetail()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);

        QSignalSpy spy(&policy, &PolicyListController::checkFinished);
        policy.check(QStringLiteral("user"), QStringLiteral("@a:b.example"));
        Q_EMIT client.policyCheckFinished(client.lastOp,
                                          QStringLiteral("@a:b.example"),
                                          false, {});
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).toBool(), false);
        QVERIFY(spy.first().at(2).toMap().isEmpty());
    }
};

QTEST_MAIN(PolicyListControllerTest)
#include "PolicyListControllerTest.moc"
