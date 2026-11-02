
#include "matrix/MockMatrixClient.h"
#include "profile/UserProfileResolver.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr int kSignalTimeoutMs = 3000;

bool login(MockMatrixClient &client)
{
    QSignalSpy spy(&client, &MatrixClient::loginSucceeded);
    client.login(QStringLiteral("https://mock.local"),
                 QStringLiteral("alice"), QStringLiteral("x"));
    return spy.wait(kSignalTimeoutMs);
}
}

class UserProfileResolverTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void asksOnceAndAnswersFromTheCacheAfterwards()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockDisplayNames.insert(QStringLiteral("@dim:example.org"),
                                       QStringLiteral("dim"));
        client.mockAvatarUrls.insert(QStringLiteral("@dim:example.org"),
                                     QStringLiteral("mxc://example.org/dim"));
        UserProfileResolver resolver;
        resolver.setClient(&client);
        QSignalSpy resolved(&resolver, &UserProfileResolver::resolved);

        const QVariantMap first = resolver.lookup(QStringLiteral("@dim:example.org"));
        QVERIFY(!first.value(QStringLiteral("known")).toBool());
        QCOMPARE(resolver.inFlightCount(), 1);
        resolver.lookup(QStringLiteral("@dim:example.org"));
        QCOMPARE(resolver.inFlightCount(), 1);

        QVERIFY(resolved.wait(kSignalTimeoutMs));
        QCOMPARE(resolved.count(), 1);
        QCOMPARE(resolved.first().at(0).toString(), QStringLiteral("@dim:example.org"));
        QCOMPARE(resolved.first().at(1).toString(), QStringLiteral("dim"));
        QCOMPARE(resolved.first().at(2).toString(), QStringLiteral("mxc://example.org/dim"));
        QCOMPARE(resolver.inFlightCount(), 0);

        const UserProfileResolver::Profile p =
            resolver.profile(QStringLiteral("@dim:example.org"));
        QVERIFY(p.known);
        QCOMPARE(p.displayName, QStringLiteral("dim"));
        QCOMPARE(p.avatarUrl, QStringLiteral("mxc://example.org/dim"));

        const QVariantMap again = resolver.lookup(QStringLiteral("@dim:example.org"));
        QVERIFY(again.value(QStringLiteral("known")).toBool());
        QCOMPARE(again.value(QStringLiteral("displayName")).toString(),
                 QStringLiteral("dim"));
        QCOMPARE(resolver.inFlightCount(), 0);
        QCOMPARE(resolved.count(), 1);
    }

    void aRefusedAnswerIsRememberedAndRetriedOnlyLater()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        UserProfileResolver resolver;
        resolver.setClient(&client);
        QSignalSpy resolved(&resolver, &UserProfileResolver::resolved);

        resolver.request(QStringLiteral("@nobody:example.org"));
        QCOMPARE(resolver.inFlightCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(resolver.inFlightCount(), 0, kSignalTimeoutMs);
        QCOMPARE(resolved.count(), 0);
        QVERIFY(!resolver.profile(QStringLiteral("@nobody:example.org")).known);

        resolver.request(QStringLiteral("@nobody:example.org"));
        QCOMPARE(resolver.inFlightCount(), 0);

        resolver.setFailureRetryForTest(0);
        resolver.request(QStringLiteral("@nobody:example.org"));
        QCOMPARE(resolver.inFlightCount(), 1);
    }

    void somebodyElsesFetchNeverLandsInTheCache()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockDisplayNames.insert(QStringLiteral("@bob:example.org"),
                                       QStringLiteral("Bob"));
        UserProfileResolver resolver;
        resolver.setClient(&client);
        QSignalSpy resolved(&resolver, &UserProfileResolver::resolved);
        QSignalSpy finished(&client, &MatrixClient::userProfileFinished);

        QVERIFY(client.fetchUserProfile(QStringLiteral("@bob:example.org")) != 0);
        QVERIFY(finished.wait(kSignalTimeoutMs));
        QCOMPARE(resolved.count(), 0);
        QVERIFY(!resolver.profile(QStringLiteral("@bob:example.org")).known);
        QCOMPARE(resolver.cachedCount(), 0);
    }

    void onlyMatrixUserIdsAreAsked()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        UserProfileResolver resolver;
        resolver.setClient(&client);
        resolver.request(QString());
        resolver.request(QStringLiteral("dim"));
        resolver.request(QStringLiteral("@dim"));
        resolver.request(QStringLiteral("!room:example.org"));
        QCOMPARE(resolver.inFlightCount(), 0);
    }

    void aNewClientDropsEverything()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockDisplayNames.insert(QStringLiteral("@dim:example.org"),
                                       QStringLiteral("dim"));
        UserProfileResolver resolver;
        resolver.setClient(&client);
        QSignalSpy resolved(&resolver, &UserProfileResolver::resolved);
        resolver.request(QStringLiteral("@dim:example.org"));
        QVERIFY(resolved.wait(kSignalTimeoutMs));
        QCOMPARE(resolver.cachedCount(), 1);

        MockMatrixClient other;
        resolver.setClient(&other);
        QCOMPARE(resolver.cachedCount(), 0);
        QVERIFY(!resolver.profile(QStringLiteral("@dim:example.org")).known);
    }

    void signingOutDropsTheProfilesTheAccountResolved()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockDisplayNames.insert(QStringLiteral("@dim:example.org"),
                                       QStringLiteral("dim"));
        client.mockAvatarUrls.insert(QStringLiteral("@dim:example.org"),
                                     QStringLiteral("mxc://example.org/dim"));
        UserProfileResolver resolver;
        resolver.setClient(&client);
        QSignalSpy resolved(&resolver, &UserProfileResolver::resolved);
        resolver.request(QStringLiteral("@dim:example.org"));
        QVERIFY(resolved.wait(kSignalTimeoutMs));
        QCOMPARE(resolver.cachedCount(), 1);

        client.logout();
        QCOMPARE(resolver.cachedCount(), 0);
        QVERIFY(!resolver.profile(QStringLiteral("@dim:example.org")).known);
        const QVariantMap after =
            resolver.lookup(QStringLiteral("@dim:example.org"));
        QVERIFY(!after.value(QStringLiteral("known")).toBool());
        QVERIFY(after.value(QStringLiteral("displayName")).toString().isEmpty());
        QVERIFY(after.value(QStringLiteral("avatarUrl")).toString().isEmpty());
    }

    void signingOutAlsoDropsARememberedRefusal()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        UserProfileResolver resolver;
        resolver.setClient(&client);
        resolver.request(QStringLiteral("@nobody:example.org"));
        QCOMPARE(resolver.inFlightCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(resolver.inFlightCount(), 0,
                                  kSignalTimeoutMs);
        resolver.request(QStringLiteral("@nobody:example.org"));
        QCOMPARE(resolver.inFlightCount(), 0);

        client.logout();
        QVERIFY(login(client));
        resolver.request(QStringLiteral("@nobody:example.org"));
        QCOMPARE(resolver.inFlightCount(), 1);
    }
};

QTEST_MAIN(UserProfileResolverTest)
#include "UserProfileResolverTest.moc"
