
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "app/SettingsManager.h"
#include "matrix/MockMatrixClient.h"
#include "presence/PresenceManager.h"

namespace {

struct RecordedRequest {
    QStringList userIds;
    quint64 opId = 0;
};

class FakePresenceClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsPresence() const override { return supports; }
    QString currentUserId() const override { return self; }
    void requestPresence(const QStringList &userIds, quint64 opId) override
    {
        requests.append({ userIds, opId });
    }
    void publishPresence(int state) override { published.append(state); }

    bool supports = true;
    QString self = QStringLiteral("@me:example.org");
    QList<RecordedRequest> requests;
    QList<int> published;
};

QVariantMap okEntry(const QString &userId, const QString &state,
                    bool active = true, qlonglong ago = 5000)
{
    return QVariantMap{
        { QStringLiteral("userId"), userId },
        { QStringLiteral("ok"), true },
        { QStringLiteral("state"), state },
        { QStringLiteral("currentlyActive"), active },
        { QStringLiteral("lastActiveAgoMs"), ago },
    };
}

QVariantMap failEntry(const QString &userId, const QString &category)
{
    return QVariantMap{
        { QStringLiteral("userId"), userId },
        { QStringLiteral("ok"), false },
        { QStringLiteral("category"), category },
    };
}

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString repositoryRoot()
{
    for (const QString &start :
         { QCoreApplication::applicationDirPath(), QDir::currentPath() }) {
        QDir dir(start);
        for (int depth = 0; depth < 8; ++depth) {
            const bool found =
                QFileInfo::exists(dir.absoluteFilePath(QStringLiteral(
                    "src/matrix/RustSdkMatrixClient.h")))
                && QFileInfo::exists(dir.absoluteFilePath(
                    QStringLiteral("rust/src/presence.rs")));
            if (found)
                return dir.absolutePath();
            if (!dir.cdUp())
                break;
        }
    }
    return {};
}

}

class PresenceManagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void watchTriggersOneDebouncedBurst();
    void incomingActivityRepollsWatchedCachedSender();
    void batchUpdatesCacheAndRevision();
    void absentLastActiveStaysUnknown();
    void staleOpIdIsIgnored();
    void transientFailureKeepsLastKnownState();
    void authoritativeAbsenceErases();
    void allForbiddenBatchesLatchServerRefusal();
    void singleUserForbiddenNeverLatches();
    void loggedOutClearsSessionAndResetsLatch();
    void loggedOutDropsWatchedSet();
    void syncingEdgePublishesAndPolls();
    void aRepeatedSyncingEdgeDoesNotRepublishTheSameState();
    void aRateLimitedPublishRetriesInsideTheExpiryWindow();
    void anUnretryableRejectionDoesNotArmARetry();
    void backgroundGraceKeepsOnlineUntilIdleDwell();
    void batchRotationCoversWatchedSetBeyondCap();
    void disablingShareSettingPublishesOfflineOnce();
    void pendingFinalOfflineFlushesOnSyncEdge();
    void unsupportedBackendStaysInactive();

    void unknownPresenceIsNotUnavailable();
    void unavailableIsFalseWithoutAClient();
    void unsupportedBackendReportsUnavailable();
    void latchArmingReportsUnavailableAndSessionEndClearsIt();
    void forbiddenBatchBelowMinimumNeitherAdvancesNorResetsTheLatch();
    void repeatedForbiddenEntriesForOneUserNeverLatch();
    void flatOfflineServerNeverLatchesOrReportsUnavailable();
    void singleForbiddenErasesTheCachedState();
    void watchIsRefCountedPerHolder();
    void watchesQueuedBeforeSyncingWaitForTheSyncingEdge();
    void stayingInSyncingDoesNotRepoll();
    void replayedAnswerForTheSameRoundIsDropped();
    void answerArrivingAfterSignOutIsDropped();
    void switchingAccountDropsTheWatchedSetAndBumpsTheEpoch();
    void presenceIsCompiledInWithNoPlatformConditional();

    void ownPresenceComesFromWhatThisClientPublishes();

    void typingWithdrawsAContradictedOfflineWithoutFabricatingOnline();
    void typingNeverTouchesOnlineAwayOrAnUnknownUser();
    void typingEvidenceExpiresAndTheDotComesBackOnItsOwn();
    void typingRepollsTheUserAndSignsOutWithTheSession();

private:
    void goSyncing(FakePresenceClient &client)
    {
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    }

    void reconnect(FakePresenceClient &client)
    {
        Q_EMIT client.connectionStateChanged(MatrixClient::Error);
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    }

    QTemporaryDir m_configHome;
};

void PresenceManagerTest::initTestCase()
{
    QVERIFY(m_configHome.isValid());
    qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("presence-manager-test"));
}

void PresenceManagerTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void PresenceManagerTest::watchTriggersOneDebouncedBurst()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);

    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    presence.watch(QStringLiteral("@alice:example.org"));

    QTRY_COMPARE(client.requests.size(), 1);
    QCOMPARE(client.requests.first().userIds.size(), 2);
    QVERIFY(client.requests.first().userIds.contains(
        QStringLiteral("@alice:example.org")));
    QVERIFY(client.requests.first().userIds.contains(
        QStringLiteral("@bob:example.org")));
}

void PresenceManagerTest::incomingActivityRepollsWatchedCachedSender()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("offline"), false) });
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("offline"));

    presence.noteActivity(QStringLiteral("@alice:example.org"));
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("offline"));
    QTRY_COMPARE(client.requests.size(), 2);
    QCOMPARE(client.requests.last().userIds,
             (QStringList{ QStringLiteral("@alice:example.org") }));
}

void PresenceManagerTest::batchUpdatesCacheAndRevision()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    QSignalSpy revisions(&presence, &PresenceManager::revisionChanged);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("online")) });

    QCOMPARE(revisions.count(), 1);
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("online"));
    const QVariantMap info =
        presence.infoFor(QStringLiteral("@alice:example.org"));
    QCOMPARE(info.value(QStringLiteral("state")).toString(),
             QStringLiteral("online"));
    QCOMPARE(info.value(QStringLiteral("currentlyActive")).toBool(), true);
    QVERIFY(info.value(QStringLiteral("lastActiveAgoMs")).toLongLong()
            >= 5000);
    QCOMPARE(presence.stateFor(QStringLiteral("@nobody:example.org")),
             QString());
    QVERIFY(presence.infoFor(QStringLiteral("@nobody:example.org")).isEmpty());
}

void PresenceManagerTest::absentLastActiveStaysUnknown()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("offline"), false, -1) });
    const QVariantMap info =
        presence.infoFor(QStringLiteral("@alice:example.org"));
    QCOMPARE(info.value(QStringLiteral("state")).toString(),
             QStringLiteral("offline"));
    QCOMPARE(info.value(QStringLiteral("lastActiveAgoMs")).toLongLong(),
             qlonglong(-1));
}

void PresenceManagerTest::staleOpIdIsIgnored()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    QSignalSpy revisions(&presence, &PresenceManager::revisionChanged);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId + 999,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("online")) });
    QCOMPARE(revisions.count(), 0);
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
}

void PresenceManagerTest::transientFailureKeepsLastKnownState()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("online")) });

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@alice:example.org"),
                    QStringLiteral("network")) });
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("online"));
}

void PresenceManagerTest::authoritativeAbsenceErases()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("online")) });

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@alice:example.org"),
                    QStringLiteral("not_found")) });
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
}

void PresenceManagerTest::allForbiddenBatchesLatchServerRefusal()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    QVERIFY(presence.active());

    const QVariantList bothForbidden{
        failEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("forbidden")),
        failEntry(QStringLiteral("@bob:example.org"),
                  QStringLiteral("forbidden")),
    };
    QSignalSpy activeSpy(&presence, &PresenceManager::activeChanged);
    Q_EMIT client.presenceReceived(client.requests.first().opId,
                                   bothForbidden);
    QVERIFY(presence.active());

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   bothForbidden);
    QVERIFY(!presence.active());
    QCOMPARE(activeSpy.count(), 1);
    QVERIFY(presence.supported());

    const int before = client.requests.size();
    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTest::qWait(50);
    QCOMPARE(client.requests.size(), before);
}

void PresenceManagerTest::singleUserForbiddenNeverLatches()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    for (int round = 0; round < 3; ++round) {
        Q_EMIT client.presenceReceived(
            client.requests.last().opId,
            { failEntry(QStringLiteral("@alice:example.org"),
                        QStringLiteral("forbidden")) });
        QVERIFY(presence.active());
        Q_EMIT client.connectionStateChanged(MatrixClient::Error);
        goSyncing(client);
        QTRY_COMPARE(client.requests.size(), round + 2);
    }
    QVERIFY(presence.active());
}

void PresenceManagerTest::loggedOutClearsSessionAndResetsLatch()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("online")) });
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("online"));

    QSignalSpy revisions(&presence, &PresenceManager::revisionChanged);
    Q_EMIT client.loggedOut();
    QCOMPARE(revisions.count(), 1);
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
    QVERIFY(presence.active());
}

void PresenceManagerTest::loggedOutDropsWatchedSet()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    QSignalSpy epochs(&presence, &PresenceManager::sessionEpochChanged);
    Q_EMIT client.loggedOut();
    QCOMPARE(epochs.count(), 1);

    goSyncing(client);
    QTest::qWait(500);
    QCOMPARE(client.requests.size(), 1);
}

void PresenceManagerTest::backgroundGraceKeepsOnlineUntilIdleDwell()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setApplicationActive(true);
    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 0 }));

    presence.setIdleThresholdForTest(200);
    presence.setPublishIntervalForTest(50);
    QTest::qWait(400);
    QVERIFY(!client.published.contains(1));

    presence.setApplicationActive(false);
    QVERIFY(!client.published.contains(1));

    QTRY_VERIFY(client.published.contains(1));
}

void PresenceManagerTest::aRepeatedSyncingEdgeDoesNotRepublishTheSameState()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setApplicationActive(true);
    presence.setMinPublishGapForTest(5000);
    presence.setPublishIntervalForTest(1000 * 1000);

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 0 }));

    for (int i = 0; i < 2; ++i) {
        Q_EMIT client.connectionStateChanged(MatrixClient::Connecting);
        goSyncing(client);
    }
    QVERIFY2(client.published == (QList<int>{ 0 }),
             "a repeated syncing edge re-sent an unchanged presence, which "
             "is the PUT the server rate-limits");

    presence.setIdleThresholdForTest(50);
    presence.setApplicationActive(false);
    QTest::qWait(150);
    Q_EMIT client.connectionStateChanged(MatrixClient::Connecting);
    goSyncing(client);
    QVERIFY2(client.published.contains(1),
             "the guard swallowed a genuine change to Away");
}

void PresenceManagerTest::aRateLimitedPublishRetriesInsideTheExpiryWindow()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setApplicationActive(true);
    presence.setMinPublishGapForTest(60 * 1000);
    presence.setPublishIntervalForTest(120 * 1000);

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 0 }));
    QCOMPARE(presence.publishAttempts(), 1);
    QCOMPARE(presence.publishRejections(), 0);

    Q_EMIT client.presencePublishFailed(QStringLiteral("rate_limited"), 1);
    QCOMPARE(presence.publishRejections(), 1);
    QCOMPARE(presence.retryChainForTest(), 1);
    QTest::qWait(300);
    QVERIFY2(client.published == (QList<int>{ 0 }),
             "the retry fired inside 300 ms — the floor did not bound a "
             "near-zero server hint");

    QTRY_VERIFY_WITH_TIMEOUT(client.published.size() == 2, 12 * 1000);
    QCOMPARE(presence.publishAttempts(), 2);
    QCOMPARE(client.published.last(), 0);

    QCOMPARE(presence.retryChainForTest(), 1);
    for (int i = 0; i < 10; ++i) {
        Q_EMIT client.presencePublishFailed(QStringLiteral("rate_limited"), 1);
        QTest::qWait(20);
        QVERIFY2(presence.retryChainForTest() <= PresenceManager::maxRetryChain(),
                 qPrintable(QStringLiteral("the retry chain ran past its cap "
                                           "to %1")
                                .arg(presence.retryChainForTest())));
    }
    QCOMPARE(presence.retryChainForTest(), PresenceManager::maxRetryChain());
    const int owed = client.published.size();
    Q_EMIT client.presencePublishFailed(QStringLiteral("rate_limited"), 1);
    QCOMPARE(presence.retryChainForTest(), PresenceManager::maxRetryChain());
    QTest::qWait(8 * 1000);
    QVERIFY2(client.published.size() - owed <= 1,
             qPrintable(QStringLiteral("the chain kept publishing past its "
                                       "cap: %1 more")
                            .arg(client.published.size() - owed)));
    QVERIFY(presence.publishRejections() >= 12);
}

void PresenceManagerTest::anUnretryableRejectionDoesNotArmARetry()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setApplicationActive(true);
    presence.setMinPublishGapForTest(60 * 1000);
    presence.setPublishIntervalForTest(120 * 1000);

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 0 }));

    Q_EMIT client.presencePublishFailed(QStringLiteral("forbidden"), 0);
    QCOMPARE(presence.retryChainForTest(), 0);
    QTest::qWait(1200);
    QVERIFY2(client.published == (QList<int>{ 0 }),
             "a forbidden rejection armed a retry");
    QCOMPARE(presence.publishRejections(), 1);
}

void PresenceManagerTest::syncingEdgePublishesAndPolls()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);

    QVERIFY(settings.sharePresence());
    QCOMPARE(client.published.size(), 0);
    goSyncing(client);
    QCOMPARE(client.published.size(), 1);
    QCOMPARE(client.published.first(), 0);

    goSyncing(client);
    QCOMPARE(client.published.size(), 1);

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QCOMPARE(client.published.size(), 1);

    presence.setMinPublishGapForTest(50);
    QTest::qWait(80);
    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QCOMPARE(client.published.size(), 2);
}

void PresenceManagerTest::batchRotationCoversWatchedSetBeyondCap()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);

    QStringList all;
    for (int i = 0; i < 45; ++i) {
        const QString userId =
            QStringLiteral("@user%1:example.org").arg(i, 3, 10, QLatin1Char('0'));
        all.append(userId);
        presence.watch(userId);
    }
    QTRY_COMPARE(client.requests.size(), 1);
    QCOMPARE(client.requests.first().userIds.size(), 40);

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTRY_COMPARE(client.requests.size(), 3);

    QSet<QString> covered;
    for (const QString &id : client.requests.at(1).userIds)
        covered.insert(id);
    for (const QString &id : client.requests.at(2).userIds)
        covered.insert(id);
    for (const QString &id : all)
        QVERIFY2(covered.contains(id), qPrintable(id));
}

void PresenceManagerTest::disablingShareSettingPublishesOfflineOnce()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    goSyncing(client);
    QCOMPARE(client.published.size(), 1);

    settings.setSharePresence(false);
    QCOMPARE(client.published.size(), 2);
    QCOMPARE(client.published.last(), 2);

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QCOMPARE(client.published.size(), 2);

    settings.setSharePresence(true);
    QCOMPARE(client.published.size(), 3);
    QCOMPARE(client.published.last(), 0);
}

void PresenceManagerTest::pendingFinalOfflineFlushesOnSyncEdge()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);

    settings.setSharePresence(false);
    QCOMPARE(client.published.size(), 0);

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 2 }));

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 2 }));
}

void PresenceManagerTest::unsupportedBackendStaysInactive()
{
    FakePresenceClient client;
    client.supports = false;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    QVERIFY(!presence.active());
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTest::qWait(500);
    QCOMPARE(client.requests.size(), 0);
    QCOMPARE(client.published.size(), 0);
}

void PresenceManagerTest::unknownPresenceIsNotUnavailable()
{
    FakePresenceClient client;
    PresenceManager presence;
    QSignalSpy unavailableSpy(&presence, &PresenceManager::unavailableChanged);
    presence.setClient(&client);
    QCOMPARE(unavailableSpy.count(), 1);
    QVERIFY(presence.supported());
    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());

    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
    QVERIFY(presence.infoFor(QStringLiteral("@alice:example.org")).isEmpty());
    QVERIFY(!presence.unavailable());

    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@alice:example.org"),
                    QStringLiteral("network")) });
    QVERIFY(!presence.unavailable());

    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@alice:example.org"),
                    QStringLiteral("forbidden")) });
    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());
    QCOMPARE(unavailableSpy.count(), 1);
}

void PresenceManagerTest::unavailableIsFalseWithoutAClient()
{
    PresenceManager presence;
    QVERIFY(!presence.supported());
    QVERIFY(!presence.active());
    QVERIFY(!presence.unavailable());
}

void PresenceManagerTest::unsupportedBackendReportsUnavailable()
{
    FakePresenceClient client;
    client.supports = false;
    PresenceManager presence;
    QSignalSpy unavailableSpy(&presence, &PresenceManager::unavailableChanged);
    presence.setClient(&client);
    QVERIFY(presence.unavailable());
    QVERIFY(!presence.supported());
    QVERIFY(!presence.active());
    QCOMPARE(unavailableSpy.count(), 1);
}

void PresenceManagerTest::latchArmingReportsUnavailableAndSessionEndClearsIt()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    QSignalSpy unavailableSpy(&presence, &PresenceManager::unavailableChanged);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    const QVariantList bothForbidden{
        failEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("forbidden")),
        failEntry(QStringLiteral("@bob:example.org"),
                  QStringLiteral("forbidden")),
    };
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   bothForbidden);
    QVERIFY(!presence.unavailable());

    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   bothForbidden);
    QVERIFY(presence.unavailable());
    QCOMPARE(unavailableSpy.count(), 1);
    QVERIFY(presence.supported());

    presence.watch(QStringLiteral("@carol:example.org"));
    QTest::qWait(500);
    QCOMPARE(client.requests.size(), 2);

    Q_EMIT client.loggedOut();
    QVERIFY(!presence.unavailable());
    QCOMPARE(unavailableSpy.count(), 2);
}

void PresenceManagerTest::forbiddenBatchBelowMinimumNeitherAdvancesNorResetsTheLatch()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    const QVariantList broadForbidden{
        failEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("forbidden")),
        failEntry(QStringLiteral("@bob:example.org"),
                  QStringLiteral("forbidden")),
    };

    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   broadForbidden);
    QVERIFY(presence.active());

    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@carol:example.org"),
                    QStringLiteral("forbidden")) });
    QVERIFY(presence.active());

    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 3);
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   broadForbidden);
    QVERIFY(!presence.active());
    QVERIFY(presence.unavailable());
}

void PresenceManagerTest::repeatedForbiddenEntriesForOneUserNeverLatch()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    const QVariantList sameUserTwice{
        failEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("forbidden")),
        failEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("forbidden")),
    };
    for (int round = 0; round < 3; ++round) {
        Q_EMIT client.presenceReceived(client.requests.last().opId,
                                       sameUserTwice);
        QVERIFY(presence.active());
        QVERIFY(!presence.unavailable());
        reconnect(client);
        QTRY_COMPARE(client.requests.size(), round + 2);
    }
    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());
}

void PresenceManagerTest::flatOfflineServerNeverLatchesOrReportsUnavailable()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    const QVariantList allOffline{
        okEntry(QStringLiteral("@alice:example.org"),
                QStringLiteral("offline"), false),
        okEntry(QStringLiteral("@bob:example.org"),
                QStringLiteral("offline"), false),
    };
    Q_EMIT client.presenceReceived(client.requests.last().opId, allOffline);
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(client.requests.last().opId, allOffline);

    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("offline"));
}

void PresenceManagerTest::singleForbiddenErasesTheCachedState()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    Q_EMIT client.presenceReceived(
        client.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("online")) });
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("online"));

    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@alice:example.org"),
                    QStringLiteral("forbidden")) });
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
    QVERIFY(presence.infoFor(QStringLiteral("@alice:example.org")).isEmpty());
    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());
}

void PresenceManagerTest::watchIsRefCountedPerHolder()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    QCOMPARE(client.requests.first().userIds.size(), 2);

    presence.unwatch(QStringLiteral("@alice:example.org"));
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    QVERIFY(client.requests.last().userIds.contains(
        QStringLiteral("@alice:example.org")));

    presence.unwatch(QStringLiteral("@alice:example.org"));
    presence.unwatch(QStringLiteral("@bob:example.org"));
    presence.unwatch(QStringLiteral("@alice:example.org"));
    presence.unwatch(QStringLiteral("@nobody:example.org"));
    reconnect(client);
    QTest::qWait(50);
    QCOMPARE(client.requests.size(), 2);
}

void PresenceManagerTest::watchesQueuedBeforeSyncingWaitForTheSyncingEdge()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTest::qWait(500);
    QCOMPARE(client.requests.size(), 0);

    goSyncing(client);
    QTRY_COMPARE(client.requests.size(), 1);
    QCOMPARE(client.requests.first().userIds.size(), 2);
    QVERIFY(client.requests.first().userIds.contains(
        QStringLiteral("@alice:example.org")));
    QVERIFY(client.requests.first().userIds.contains(
        QStringLiteral("@bob:example.org")));
}

void PresenceManagerTest::stayingInSyncingDoesNotRepoll()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    goSyncing(client);
    goSyncing(client);
    QTest::qWait(50);
    QCOMPARE(client.requests.size(), 1);

    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    QCOMPARE(client.requests.last().userIds,
             (QStringList{ QStringLiteral("@alice:example.org") }));
}

void PresenceManagerTest::replayedAnswerForTheSameRoundIsDropped()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    const quint64 opId = client.requests.first().opId;
    Q_EMIT client.presenceReceived(
        opId, { okEntry(QStringLiteral("@alice:example.org"),
                        QStringLiteral("online")) });
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("online"));

    QSignalSpy revisions(&presence, &PresenceManager::revisionChanged);
    Q_EMIT client.presenceReceived(
        opId, { okEntry(QStringLiteral("@alice:example.org"),
                        QStringLiteral("offline"), false) });
    QCOMPARE(revisions.count(), 0);
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QStringLiteral("online"));
}

void PresenceManagerTest::answerArrivingAfterSignOutIsDropped()
{
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    const quint64 opId = client.requests.first().opId;

    Q_EMIT client.loggedOut();
    QSignalSpy revisions(&presence, &PresenceManager::revisionChanged);
    Q_EMIT client.presenceReceived(
        opId, { okEntry(QStringLiteral("@alice:example.org"),
                        QStringLiteral("online")) });
    QCOMPARE(revisions.count(), 0);
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
}

void PresenceManagerTest::switchingAccountDropsTheWatchedSetAndBumpsTheEpoch()
{
    FakePresenceClient first;
    FakePresenceClient second;
    PresenceManager presence;
    presence.setClient(&first);
    goSyncing(first);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(first.requests.size(), 1);
    Q_EMIT first.presenceReceived(
        first.requests.first().opId,
        { okEntry(QStringLiteral("@alice:example.org"),
                  QStringLiteral("online")) });

    QSignalSpy epochs(&presence, &PresenceManager::sessionEpochChanged);
    presence.setClient(&second);
    QCOMPARE(epochs.count(), 1);
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());

    goSyncing(second);
    QTest::qWait(500);
    QCOMPARE(second.requests.size(), 0);
    QCOMPARE(first.requests.size(), 1);
}

void PresenceManagerTest::presenceIsCompiledInWithNoPlatformConditional()
{
    const QString root = repositoryRoot();
    QVERIFY2(!root.isEmpty(), "could not locate the repository to scan");

    const QString header =
        readText(root + QStringLiteral("/src/matrix/RustSdkMatrixClient.h"));
    QVERIFY(!header.isEmpty());
    QVERIFY2(header.contains(QStringLiteral(
                 "bool supportsPresence() const override { return true; }")),
             "the Rust backend must advertise presence unconditionally");
    QVERIFY(!header.contains(QStringLiteral("__APPLE__")));
    QVERIFY(!header.contains(QStringLiteral("Q_OS_MAC")));

    const QStringList libLines =
        readText(root + QStringLiteral("/rust/src/lib.rs"))
            .split(QLatin1Char('\n'));
    const int modLine = libLines.indexOf(QStringLiteral("mod presence;"));
    QVERIFY2(modLine > 0, "rust/src/lib.rs must declare `mod presence;`");
    QVERIFY2(!libLines.at(modLine - 1).trimmed().startsWith(
                 QStringLiteral("#[")),
             "no attribute may gate the presence module");

    const QString cmake = readText(root + QStringLiteral("/CMakeLists.txt"));
    QVERIFY(cmake.contains(QStringLiteral("src/presence/PresenceManager.cpp")));
    const QStringList cmakeLines = cmake.split(QLatin1Char('\n'));
    for (const QString &line : cmakeLines) {
        if (!line.contains(QStringLiteral("APPLE")))
            continue;
        QVERIFY2(!line.contains(QStringLiteral("presence"), Qt::CaseInsensitive),
                 qPrintable(line));
    }
}

void PresenceManagerTest::ownPresenceComesFromWhatThisClientPublishes()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);

    const QString me = client.self;
    QCOMPARE(presence.stateFor(me), QString());

    goSyncing(client);
    QCOMPARE(client.published.size(), 1);
    QCOMPARE(presence.stateFor(me), QStringLiteral("online"));
    const QVariantMap info = presence.infoFor(me);
    QCOMPARE(info.value(QStringLiteral("state")).toString(),
             QStringLiteral("online"));
    QCOMPARE(info.value(QStringLiteral("lastActiveAgoMs")).toLongLong(), 0LL);

    presence.watch(me);
    QTRY_VERIFY(!client.requests.isEmpty());
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { okEntry(me, QStringLiteral("offline")) });
    QCOMPARE(presence.stateFor(me), QStringLiteral("online"));

    settings.setSharePresence(false);
    QCOMPARE(presence.stateFor(me), QStringLiteral("offline"));

    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
}

void PresenceManagerTest::typingWithdrawsAContradictedOfflineWithoutFabricatingOnline()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    goSyncing(client);

    const QString alice = QStringLiteral("@alice:example.org");
    presence.watch(alice);
    QTRY_VERIFY(!client.requests.isEmpty());
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   { okEntry(alice, QStringLiteral("offline")) });
    QCOMPARE(presence.stateFor(alice), QStringLiteral("offline"));
    QVERIFY(!presence.infoFor(alice).isEmpty());

    QSignalSpy revisions(&presence, &PresenceManager::revisionChanged);
    presence.noteTyping(alice);

    QCOMPARE(presence.stateFor(alice), QString());
    QVERIFY2(presence.stateFor(alice) != QStringLiteral("online"),
             "typing proves activity, never a presence state");
    QVERIFY(presence.infoFor(alice).isEmpty());
    QCOMPARE(revisions.count(), 1);
}

void PresenceManagerTest::typingNeverTouchesOnlineAwayOrAnUnknownUser()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    goSyncing(client);

    const QString alice = QStringLiteral("@alice:example.org");
    const QString bob = QStringLiteral("@bob:example.org");
    const QString carol = QStringLiteral("@carol:example.org");
    presence.watch(alice);
    presence.watch(bob);
    QTRY_VERIFY(!client.requests.isEmpty());
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { okEntry(alice, QStringLiteral("online")),
          okEntry(bob, QStringLiteral("unavailable")) });

    presence.noteTyping(alice);
    presence.noteTyping(bob);
    presence.noteTyping(carol);

    QCOMPARE(presence.stateFor(alice), QStringLiteral("online"));
    QCOMPARE(presence.stateFor(bob), QStringLiteral("unavailable"));
    QCOMPARE(presence.stateFor(carol), QString());

    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   { okEntry(client.self,
                                             QStringLiteral("offline")) });
    presence.noteTyping(client.self);
    QCOMPARE(presence.stateFor(client.self), QStringLiteral("online"));
}

void PresenceManagerTest::typingEvidenceExpiresAndTheDotComesBackOnItsOwn()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setTypingEvidenceWindowForTest(60);
    presence.setClient(&client);
    goSyncing(client);

    const QString alice = QStringLiteral("@alice:example.org");
    presence.watch(alice);
    QTRY_VERIFY(!client.requests.isEmpty());
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   { okEntry(alice, QStringLiteral("offline")) });

    presence.noteTyping(alice);
    QCOMPARE(presence.stateFor(alice), QString());

    QSignalSpy revisions(&presence, &PresenceManager::revisionChanged);
    QTRY_COMPARE_WITH_TIMEOUT(presence.stateFor(alice),
                              QStringLiteral("offline"), 3000);
    QVERIFY2(revisions.count() >= 1,
             "the expiry must announce itself, or the withheld dot never "
             "comes back until an unrelated update happens along");
}

void PresenceManagerTest::typingRepollsTheUserAndSignsOutWithTheSession()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    goSyncing(client);

    const QString alice = QStringLiteral("@alice:example.org");
    presence.watch(alice);
    QTRY_VERIFY(!client.requests.isEmpty());
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   { okEntry(alice, QStringLiteral("offline")) });
    const int before = client.requests.size();

    presence.noteTyping(alice);
    QTRY_VERIFY(client.requests.size() > before);
    QVERIFY(client.requests.last().userIds.contains(alice));

    QCOMPARE(presence.stateFor(alice), QString());
    Q_EMIT client.loggedOut();
    presence.watch(alice);
    goSyncing(client);
    QTRY_VERIFY(!client.requests.isEmpty());
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   { okEntry(alice, QStringLiteral("offline")) });
    QCOMPARE(presence.stateFor(alice), QStringLiteral("offline"));
}

QTEST_MAIN(PresenceManagerTest)
#include "PresenceManagerTest.moc"
