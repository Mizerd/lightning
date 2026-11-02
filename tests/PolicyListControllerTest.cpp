
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

}

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

        Q_EMIT client.policyRulesReceived(slowOp, true, kRoomA, true, false,
                                          oneRule());
        QCOMPARE(policy.rules()->rowCount(), 0);
        QCOMPARE(policy.roomId(), kRoomB);

        Q_EMIT client.policyRulesReceived(fastOp, true, kRoomB, false, false,
                                          oneRule());
        QCOMPARE(policy.rules()->rowCount(), 1);
    }

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
        QCOMPARE(client.lastEntity, QStringLiteral("bad.example"));
        QCOMPARE(client.lastRecommendation, QStringLiteral("m.ban"));

        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());
        QCOMPARE(client.fetchCalls, 2);

        policy.removeRule(QStringLiteral("server"),
                          QStringLiteral("bad.example"),
                          QStringLiteral("rule:bad.example"));
        QCOMPARE(client.writeCalls, 2);
        QVERIFY2(client.lastRecommendation.isEmpty(),
                 "removal is an EMPTY recommendation — the bridge writes an "
                 "empty state event, which is the only removal state has");
    }

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

        client.logout();
        QTest::qWait(50);
        QVERIFY(policy.subscriptions().isEmpty());
        QVERIFY(policy.roomId().isEmpty());
        QCOMPARE(policy.rules()->rowCount(), 0);
    }

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

        Q_EMIT client.policyCheckFinished(first, QStringLiteral("@a:b.example"),
                                          true, {});
        QCOMPARE(spy.count(), 0);

        Q_EMIT client.policyCheckFinished(second, QStringLiteral("@c:b.example"),
                                          false, {});
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("@c:b.example"));
    }

    void removalIsKeyedOnTheRulesOwnStateKey()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, oneRule());

        policy.removeRule(QStringLiteral("user"),
                          QStringLiteral("@spam:bad.example"),
                          QStringLiteral("mjolnir_1699"));
        QCOMPARE(client.writeCalls, 1);
        QCOMPARE(client.lastStateKey, QStringLiteral("mjolnir_1699"));

        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());
        policy.removeRule(QStringLiteral("user"),
                          QStringLiteral("@spam:bad.example"), QString());
        QCOMPARE(client.writeCalls, 1);
    }

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

        entity->setProperty("text", QStringLiteral("@spam:bad.example"));
        QVERIFY(publish(false));
        QCOMPARE(client.writeCalls, 1);
        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());

        entity->setProperty("text", QStringLiteral("*"));
        QVERIFY(!publish(false));
        QCOMPARE(client.writeCalls, 1);
        QCOMPARE(dialog->property("pendingBroadEntity").toString(), QStringLiteral("*"));
        QVERIFY(publish(true));
        QCOMPARE(client.writeCalls, 2);
        QCOMPARE(client.lastEntity, QStringLiteral("*"));
        QCOMPARE(client.lastRecommendation, QStringLiteral("m.ban"));
        QVERIFY(dialog->property("pendingBroadEntity").toString().isEmpty());
    }

    void rulesListInADeterministicOrder()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        QVariantList rules;
        for (const char *e : { "@c:x", "@a:x", "@b:x" })
            rules += oneRule(QString::fromLatin1(e));
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, rules);
        auto order = [&] {
            QStringList out;
            for (int i = 0; i < policy.rules()->rowCount(); ++i)
                out << policy.rules()->data(policy.rules()->index(i),
                                            PolicyRuleModel::EntityRole).toString();
            return out;
        };
        const QStringList expected{ "@a:x", "@b:x", "@c:x" };
        QCOMPARE(order(), expected);
    }

    void aReReadAfterRemovalEditsRowsAndNeverResets()
    {
        PolicyClient client;
        PolicyListController policy;
        policy.setClient(&client);
        policy.openRoom(kRoomA);
        QVariantList rules;
        for (const char *e : { "@a:x", "@b:x", "@c:x", "@d:x" })
            rules += oneRule(QString::fromLatin1(e));
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, rules);
        QSignalSpy resets(policy.rules(), &QAbstractItemModel::modelReset);
        QSignalSpy removed(policy.rules(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(policy.rules(), &QAbstractItemModel::rowsInserted);

        QVariantList after = rules;
        after.removeAt(3);
        policy.openRoom(kRoomA);
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, rules);
        resets.clear();
        policy.removeRule(QStringLiteral("user"), QStringLiteral("@d:x"),
                          QStringLiteral("rule:@d:x"));
        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, after);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(policy.rules()->rowCount(), 3);

        QVariantList plus = after;
        plus += oneRule(QStringLiteral("@bb:x"));
        policy.addRule(QStringLiteral("user"), QStringLiteral("@bb:x"), QString());
        Q_EMIT client.policyRuleWritten(client.lastOp, true, QString());
        Q_EMIT client.policyRulesReceived(client.lastOp, true, kRoomA, true,
                                          false, plus);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(policy.rules()->data(policy.rules()->index(2),
                                      PolicyRuleModel::EntityRole).toString(),
                 QStringLiteral("@bb:x"));
    }
};

QTEST_MAIN(PolicyListControllerTest)
#include "PolicyListControllerTest.moc"
