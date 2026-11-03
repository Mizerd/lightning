
#include "matrix/MockMatrixClient.h"
#include "profile/NameColorManager.h"

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

class NameColorManagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void askingRepeatedlyDispatchesExactlyOneFetchPerUser()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        QVERIFY(manager.available());

        for (int i = 0; i < 30; ++i)
            manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);

        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        for (int i = 0; i < 10; ++i)
            manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);
    }

    void aChangedColourReachesARunningClientOnTheNextRead()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.setRefreshIntervalForTest(60 * 1000);
        QSignalSpy revisions(&manager, &NameColorManager::revisionChanged);

        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        const int fetchesAfterFirstAnswer = client.nameColorFetches;
        const int revisionsAfterFirstAnswer = revisions.count();
        manager.setRefreshIntervalForTest(0);

        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#123456"));
        QCOMPARE(manager.colorFor(QStringLiteral("@bob:mock.local")),
                 QStringLiteral("#aabbcc"));
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, fetchesAfterFirstAnswer + 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#123456"), kSignalTimeoutMs);
        QVERIFY2(revisions.count() > revisionsAfterFirstAnswer,
                 "a changed colour must bump the revision the bindings read");
    }

    void aChangedColourReachesEveryoneOnTheSweepWithoutARerender()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.setRefreshIntervalForTest(60 * 1000);
        QSignalSpy revisions(&manager, &NameColorManager::revisionChanged);

        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        const int fetches = client.nameColorFetches;
        const int revisionsBefore = revisions.count();

        manager.sweepForTest();
        QCOMPARE(client.nameColorFetches, fetches);

        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#123456"));
        manager.setRefreshIntervalForTest(0);
        manager.sweepForTest();
        QCOMPARE(client.nameColorFetches, fetches + 1);
        QTRY_COMPARE_WITH_TIMEOUT(revisions.count(), revisionsBefore + 1,
                                  kSignalTimeoutMs);
        QCOMPARE(manager.colorFor(QStringLiteral("@bob:mock.local")),
                 QStringLiteral("#123456"));

        const int fetchesAfterSweep = client.nameColorFetches;
        manager.setRecentReadWindowForTest(0);
        QTest::qWait(5);
        manager.sweepForTest();
        QCOMPARE(client.nameColorFetches, fetchesAfterSweep);
    }

    void idleReadersAreNotPolledOnTheOldTwentySecondCadence()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        QTest::qWait(30);
        for (int i = 0; i < 5; ++i) {
            manager.sweepForTest();
            manager.colorFor(QStringLiteral("@bob:mock.local"));
        }
        QCOMPARE(client.nameColorFetches, 1);
    }

    void aProfileChangeRefetchesOnlyTheUserWhoseProfileChanged()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        client.mockNameColors.insert(QStringLiteral("@carol:mock.local"),
                                     QStringLiteral("#445566"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.setMinRefetchGapForTest(0);
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        manager.colorFor(QStringLiteral("@carol:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@carol:mock.local")),
            QStringLiteral("#445566"), kSignalTimeoutMs);
        QCOMPARE(client.nameColorFetches, 2);

        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#123456"));
        Q_EMIT client.userProfileChanged(QStringLiteral("@stranger:mock.local"));
        QCOMPARE(client.nameColorFetches, 2);
        Q_EMIT client.userProfileChanged(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 3);
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#123456"), kSignalTimeoutMs);
        QCOMPARE(manager.colorFor(QStringLiteral("@carol:mock.local")),
                 QStringLiteral("#445566"));
        QCOMPARE(client.nameColorFetches, 3);
    }

    void aProfileChangeWithinTheMinimumGapWaitsForTheNextRead()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.setMinRefetchGapForTest(60 * 1000);
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);

        for (int i = 0; i < 20; ++i)
            Q_EMIT client.userProfileChanged(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);

        manager.setMinRefetchGapForTest(0);
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 2);
    }

    void aFailedFetchKeepsTheCachedColourAndBacksOff()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.setMinRefetchGapForTest(0);
        manager.setBackoffBaseForTest(60 * 1000);
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);

        client.nameColorFetchFails = true;
        Q_EMIT client.userProfileChanged(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 2);
        QTest::qWait(40);

        for (int i = 0; i < 10; ++i) {
            Q_EMIT client.userProfileChanged(QStringLiteral("@bob:mock.local"));
            manager.colorFor(QStringLiteral("@bob:mock.local"));
            manager.sweepForTest();
        }
        QCOMPARE(client.nameColorFetches, 2);
        QCOMPARE(manager.colorFor(QStringLiteral("@bob:mock.local")),
                 QStringLiteral("#aabbcc"));

        manager.setBackoffBaseForTest(0);
        client.nameColorFetchFails = false;
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QCOMPARE(client.nameColorFetches, 3);
    }

    void aUserWithNoColourIsNotAskedAboutAgain()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        NameColorManager manager;
        manager.setClient(&client);

        QCOMPARE(manager.colorFor(QStringLiteral("@nobody:mock.local")),
                 QString());
        QTRY_COMPARE_WITH_TIMEOUT(client.nameColorFetches, 1, kSignalTimeoutMs);
        QTest::qWait(50);
        for (int i = 0; i < 10; ++i)
            manager.colorFor(QStringLiteral("@nobody:mock.local"));
        QCOMPARE(client.nameColorFetches, 1);
    }

    void aValueThatIsNotAColourNeverReachesTheCache()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        NameColorManager manager;
        manager.setClient(&client);

        const QStringList hostile = {
            QStringLiteral("red"),
            QStringLiteral("#abc"),
            QStringLiteral("#gggggg"),
            QStringLiteral("javascript:alert(1)"),
            QStringLiteral("#aabbcc; background:url(http://evil.example)"),
        };
        int user = 0;
        for (const QString &value : hostile) {
            const QString id =
                QStringLiteral("@hostile%1:mock.local").arg(user++);
            client.mockNameColors.insert(id, value);
            manager.colorFor(id);
            QTest::qWait(40);
            QVERIFY2(manager.colorFor(id).isEmpty(),
                     qPrintable(QStringLiteral("accepted %1").arg(value)));
        }
    }

    void settingOwnColourStoresItAndClearingRemovesIt()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        NameColorManager manager;
        manager.setClient(&client);
        QSignalSpy revisions(&manager, &NameColorManager::revisionChanged);

        manager.setOwnColor(QStringLiteral("#123456"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.ownColor(),
                                  QStringLiteral("#123456"), kSignalTimeoutMs);
        QVERIFY(revisions.count() > 0);

        manager.setOwnColor(QString());
        QTRY_VERIFY_WITH_TIMEOUT(manager.ownColor().isEmpty(), kSignalTimeoutMs);

        manager.setOwnColor(QStringLiteral("not a colour"));
        QCOMPARE(manager.lastError(), QStringLiteral("invalid_colour"));
        QVERIFY(manager.ownColor().isEmpty());
    }

    void anUnsupportedHomeserverStopsTheAskingAndHidesTheControl()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.nameColorsSupportedOnServer = false;
        NameColorManager manager;
        manager.setClient(&client);

        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.supported(), kSignalTimeoutMs);
        const int after = client.nameColorFetches;
        for (int i = 0; i < 10; ++i)
            manager.colorFor(QStringLiteral("@carol:mock.local"));
        QCOMPARE(client.nameColorFetches, after);
    }

    void signingOutDropsEveryCachedColour()
    {
        MockMatrixClient client;
        QVERIFY(login(client));
        client.mockNameColors.insert(QStringLiteral("@bob:mock.local"),
                                     QStringLiteral("#aabbcc"));
        NameColorManager manager;
        manager.setClient(&client);
        manager.colorFor(QStringLiteral("@bob:mock.local"));
        QTRY_COMPARE_WITH_TIMEOUT(
            manager.colorFor(QStringLiteral("@bob:mock.local")),
            QStringLiteral("#aabbcc"), kSignalTimeoutMs);

        client.logout();
        QTest::qWait(60);
        const int before = client.nameColorFetches;
        QCOMPARE(manager.colorFor(QStringLiteral("@bob:mock.local")), QString());
        QCOMPARE(client.nameColorFetches, before + 1);
    }
};

QTEST_MAIN(NameColorManagerTest)
#include "NameColorManagerTest.moc"
