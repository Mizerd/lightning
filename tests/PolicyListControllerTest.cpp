// Mjolnir-style policy lists: the controller's op discipline, and that
// following a list does not act on it.
//
// A policy room read is a whole-room /state fetch that can be slow, so
// answers arrive out of order and for rooms the user has left.

#include "app/PolicyListController.h"
#include "matrix/MockMatrixClient.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <memory>

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
    QString lastStateKey;
    void writePolicyRule(const QString &roomId, const QString &kind,
                         const QString &entity, const QString &stateKey,
                         const QString &recommendation, const QString &reason,
                         quint64 opId) override
    {
        ++writeCalls;
        lastRoom = roomId;
        lastKind = kind;
        lastEntity = entity;
        lastStateKey = stateKey;
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

// The `app` the dialog reads: only its `policy`.
class AppStub final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *policy READ policy CONSTANT)
public:
    explicit AppStub(QObject *policy) : m_policy(policy) {}
    QObject *policy() const { return m_policy; }

private:
    QObject *m_policy;
};

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

    // A slow read of one room must not land under another room's name. The
    // op id catches a superseded read of the same room, and the room id
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

    // The room-id guard on its own: an answer with the outstanding op but the
    // wrong room is refused. The op is still consumed, or `loading` would
    // hang.
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
                          QStringLiteral("bad.example"),
                          QStringLiteral("rule:bad.example"));
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

    // Following a list produces no enforcement of its own: a subscribed list
    // is somebody else's judgement.
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

    // The check's answer is forwarded, and a superseded one is not. (Stripping
    // a miss's rule detail happens in RustSdkMatrixClient's poll dispatch,
    // which this suite cannot reach.)
    void aCheckAnswersOnceAndASupersededOneIsDropped()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);

        QSignalSpy spy(&policy, &PolicyListController::checkFinished);
        policy.check(QStringLiteral("user"), QStringLiteral("@a:b.example"));
        const quint64 first = client.lastOp;
        policy.check(QStringLiteral("user"), QStringLiteral("@c:b.example"));
        const quint64 second = client.lastOp;
        QVERIFY(first != second);

        // The first question's answer arrives after the user asked a
        // second: dropped, or the UI reports the wrong person's status.
        Q_EMIT client.policyCheckFinished(first, QStringLiteral("@a:b.example"),
                                          true, {});
        QCOMPARE(spy.count(), 0);

        Q_EMIT client.policyCheckFinished(second, QStringLiteral("@c:b.example"),
                                          false, {});
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("@c:b.example"));
    }

    // Removal uses the rule's own state key, not `rule:<entity>` derived from
    // the entity: the key is a free string, and a write to a derived key
    // would succeed while the real rule stayed on the list.
    void removalIsKeyedOnTheRulesOwnStateKey()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, oneRule());

        // A rule whose key is NOT the derived form — which is exactly the
        // case the derived key would silently fail on.
        policy.removeRule(QStringLiteral("user"),
                          QStringLiteral("@spam:bad.example"),
                          QStringLiteral("mjolnir_1699"));
        QCOMPARE(client.writeCalls, 1);
        QCOMPARE(client.lastStateKey, QStringLiteral("mjolnir_1699"));

        // And with no key at all nothing is sent: the caller has not said
        // WHICH rule, and guessing is the defect.
        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());
        policy.removeRule(QStringLiteral("user"),
                          QStringLiteral("@spam:bad.example"), QString());
        QCOMPARE(client.writeCalls, 1);
    }

    // A NEW rule is the opposite case: no key, so the bridge derives the
    // conventional one — which is what lets other tools replace this rule
    // rather than add a second beside it.
    void publishingUsesTheConventionalKeyByLeavingItEmpty()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, {});

        policy.addRule(QStringLiteral("user"), QStringLiteral("@a:b.example"),
                       QString());
        QCOMPARE(client.writeCalls, 1);
        QVERIFY2(client.lastStateKey.isEmpty(),
                 "a new rule must let the bridge derive the conventional key");
    }

    // Break-it B105: a rule whose entity matches everyone (`*`) bans the
    // whole kind for every follower of the list. Legitimate in Mjolnir-style
    // lists, so the dialog asks once more instead of refusing; an ordinary
    // entity publishes on the first click.
    void anAllMatchingEntityNeedsASecondExplicitPublish()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, {});

        QQmlEngine engine;
        AppStub app(&policy);
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &app);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport MatrixClient\nPolicyListDialog {}\n",
                          QUrl(QStringLiteral("qrc:/policyListDialogTest.qml")));
        QVERIFY2(component.errors().isEmpty(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> dialog(component.create(engine.rootContext()));
        QVERIFY(dialog);
        QObject *entity = dialog->findChild<QObject *>(QStringLiteral("policyEntityField"));
        if (!entity) {
            if (auto *content = dialog->property("contentItem").value<QObject *>())
                entity = content->findChild<QObject *>(QStringLiteral("policyEntityField"));
        }
        QVERIFY2(entity, "the entity field is gone — re-anchor this case");

        auto matchesEverything = [&](const QString &text) {
            QVariant result;
            const bool invoked = QMetaObject::invokeMethod(
                dialog.get(), "entityMatchesEverything",
                Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, text));
            return invoked && result.toBool();
        };
        auto publish = [&](bool confirmed) {
            QVariant result;
            QMetaObject::invokeMethod(dialog.get(), "publishRule",
                                      Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, confirmed));
            return result.toBool();
        };

        for (const char *broad : { "*", " * ", "@*:*", "*:*", "*.*", "!*:*", "**" })
            QVERIFY2(matchesEverything(QString::fromLatin1(broad)), broad);
        for (const char *narrow : { "@spam:bad.example", "*.bad.example", "@*:bad.example",
                                    "?", "", "@spam*" })
            QVERIFY2(!matchesEverything(QString::fromLatin1(narrow)), narrow);

        // An ordinary entity goes out on the first click.
        entity->setProperty("text", QStringLiteral("@spam:bad.example"));
        QVERIFY(publish(false));
        QCOMPARE(client.writeCalls, 1);
        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());

        // `*` waits: nothing written, the confirmation is pending.
        entity->setProperty("text", QStringLiteral("*"));
        QVERIFY(!publish(false));
        QCOMPARE(client.writeCalls, 1);
        QCOMPARE(dialog->property("pendingBroadEntity").toString(), QStringLiteral("*"));
        // The explicit second Publish sends it, as typed.
        QVERIFY(publish(true));
        QCOMPARE(client.writeCalls, 2);
        QCOMPARE(client.lastEntity, QStringLiteral("*"));
        QCOMPARE(client.lastRecommendation, QStringLiteral("m.ban"));
        QVERIFY(dialog->property("pendingBroadEntity").toString().isEmpty());
    }
};

QTEST_MAIN(PolicyListControllerTest)
#include "PolicyListControllerTest.moc"
