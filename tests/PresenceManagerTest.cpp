// PresenceManager policy. The manager owns all client-side presence policy
// (sliding sync delivers no presence events, so everything rests on polling):
// watch-set bookkeeping, batch application, unknown-vs-offline, the
// disabled-server latch, session clearing and own-presence publishing. Driven
// against a fake client; real homeserver behaviour needs live validation.

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

// Both build trees live inside the source tree, so walk up from the test
// binary to find the repository. Empty on failure, which callers treat as a
// failure.
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

} // namespace

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

    // The `unavailable` property the profile popover reads, the latch's
    // distinct-user minimum, watch ref-counting, connection-state gating,
    // cross-session isolation, and the platform-independence build contract.
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

    // The local user's own presence comes from what this client publishes,
    // not the server's echo.
    void ownPresenceComesFromWhatThisClientPublishes();

    // A live typing notification withdraws a contradicted "offline": presence
    // and typing come from different sources, and the poll is the unreliable
    // half.
    void typingWithdrawsAContradictedOfflineWithoutFabricatingOnline();
    void typingNeverTouchesOnlineAwayOrAnUnknownUser();
    void typingEvidenceExpiresAndTheDotComesBackOnItsOwn();
    void typingRepollsTheUserAndSignsOutWithTheSession();

private:
    // Drives the manager to a live session and returns the fake's baseline
    // request count.
    void goSyncing(FakePresenceClient &client)
    {
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    }

    // Request another authoritative round. The scheduled timer is 30 s, so a
    // reconnect edge drives it; callers assert the resulting request count.
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
    // Second watch of the same user must not double anything.
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

    // A message from a watched sender is only a freshness hint: it queues a
    // server read instead of fabricating online.
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
    // The reported age is the server age advanced by local elapsed time,
    // never less than what the server sent.
    QVERIFY(info.value(QStringLiteral("lastActiveAgoMs")).toLongLong()
            >= 5000);
    // Unknown users read as empty, not offline.
    QCOMPARE(presence.stateFor(QStringLiteral("@nobody:example.org")),
             QString());
    QVERIFY(presence.infoFor(QStringLiteral("@nobody:example.org")).isEmpty());
}

void PresenceManagerTest::absentLastActiveStaysUnknown()
{
    // A "server sent none" age (-1, the decode default) stays -1, never
    // advanced into a fabricated "active just now".
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

    // A network error on the next round must not erase the known state.
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
    QVERIFY(presence.active()); // one batch is not enough

    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   bothForbidden);
    QVERIFY(!presence.active());
    QCOMPARE(activeSpy.count(), 1);
    QVERIFY(presence.supported()); // capability is not the latch

    // A latched manager stops polling entirely.
    const int before = client.requests.size();
    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTest::qWait(50);
    QCOMPARE(client.requests.size(), before);
}

void PresenceManagerTest::singleUserForbiddenNeverLatches()
{
    // One user's 403 (federation edge, invited-not-joined member) must not
    // disable presence for the whole session.
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
    // The account's presence must not leak into the next session.
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
    // The latch is per session: the next account's server may support
    // presence.
    QVERIFY(presence.active());
}

void PresenceManagerTest::loggedOutDropsWatchedSet()
{
    // The previous account's watch list is never polled against the next
    // account's homeserver.
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    QSignalSpy epochs(&presence, &PresenceManager::sessionEpochChanged);
    Q_EMIT client.loggedOut();
    QCOMPARE(epochs.count(), 1); // PresenceDot re-registers on this edge

    // The next session's Syncing edge polls nothing: the watched set ended
    // with the previous session.
    goSyncing(client);
    QTest::qWait(500); // outlives the burst debounce too
    QCOMPARE(client.requests.size(), 1);
}

void PresenceManagerTest::backgroundGraceKeepsOnlineUntilIdleDwell()
{
    // The idle clock measures continuous background dwell, not time since
    // focus was gained.
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
    // Holding focus past the threshold never decays towards Away.
    QTest::qWait(400);
    QVERIFY(!client.published.contains(1));

    // Losing focus does not publish Away immediately: the dwell clock starts
    // now.
    presence.setApplicationActive(false);
    QVERIFY(!client.published.contains(1));

    // Once the background dwell exceeds the threshold, the keep-alive reports
    // Away.
    QTRY_VERIFY(client.published.contains(1));
}

// A repeated Syncing edge (session start flaps through it) does not republish
// an unchanged state inside the rate window: Synapse's `rc_presence` burst is
// 1 and would reject the duplicate. A real state change is never dropped.
void PresenceManagerTest::aRepeatedSyncingEdgeDoesNotRepublishTheSameState()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setApplicationActive(true);
    // A window longer than the case, so the drop is decided by the guard, not
    // the clock.
    presence.setMinPublishGapForTest(5000);
    presence.setPublishIntervalForTest(1000 * 1000);

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 0 }));

    // Leave Syncing and come back twice, as a starting session does; nothing
    // changed, so nothing is owed.
    for (int i = 0; i < 2; ++i) {
        Q_EMIT client.connectionStateChanged(MatrixClient::Connecting);
        goSyncing(client);
    }
    QVERIFY2(client.published == (QList<int>{ 0 }),
             "a repeated syncing edge re-sent an unchanged presence, which "
             "is the PUT the server rate-limits");

    // A real change is never dropped, inside the same window.
    presence.setIdleThresholdForTest(50);
    presence.setApplicationActive(false);
    QTest::qWait(150);
    Q_EMIT client.connectionStateChanged(MatrixClient::Connecting);
    goSyncing(client);
    QVERIFY2(client.published.contains(1),
             "the guard swallowed a genuine change to Away");
}

// A rate-limited publish is retried inside the server's expiry window
// (33-63 s) rather than a full period later, and the retry bypasses
// `kMinPublishGapMs`, which only suppresses duplicates of an accepted state.
// `rc_presence` is per user, so jitter alone cannot reduce the aggregate rate.
void PresenceManagerTest::aRateLimitedPublishRetriesInsideTheExpiryWindow()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    presence.setApplicationActive(true);
    // A gap window longer than the case: the retry must bypass it.
    presence.setMinPublishGapForTest(60 * 1000);
    // A keep-alive period longer still, so only the retry can publish.
    presence.setPublishIntervalForTest(120 * 1000);

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 0 }));
    QCOMPARE(presence.publishAttempts(), 1);
    QCOMPARE(presence.publishRejections(), 0);

    // The server rejects it with a 1 ms hint, below the floor: a near-zero
    // hint must not become a busy loop.
    Q_EMIT client.presencePublishFailed(QStringLiteral("rate_limited"), 1);
    QCOMPARE(presence.publishRejections(), 1);
    QCOMPARE(presence.retryChainForTest(), 1);
    // The wait is the assertion: the emit is direct and same-thread, so no
    // timer could have fired yet. 300 ms is under the 1500 ms floor and over
    // the 1 ms hint.
    QTest::qWait(300);
    QVERIFY2(client.published == (QList<int>{ 0 }),
             "the retry fired inside 300 ms — the floor did not bound a "
             "near-zero server hint");

    // It retries on its own, well inside the 33 s the server keeps the last
    // accepted publish alive.
    QTRY_VERIFY_WITH_TIMEOUT(client.published.size() == 2, 12 * 1000);
    QCOMPARE(presence.publishAttempts(), 2);
    QCOMPARE(client.published.last(), 0);

    // The chain is bounded, asserted on the chain itself: `m_publishRetryTimer`
    // is one single-shot timer that start() restarts, so counting publishes
    // cannot show whether `kMaxRetryChain` exists.
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
    // At the cap a further rejection changes nothing: after waiting past the
    // longest interval (4 x 1500 ms plus margin), only the publish already
    // owed has happened.
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

// Only rate limiting is retried on this timescale; `forbidden` means the
// server does not do presence (the polling side has its own latch).
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
    // Asserted on the chain, not the clock: an unhinted retry would wait
    // 4000 ms, longer than any short wait here.
    QCOMPARE(presence.retryChainForTest(), 0);
    QTest::qWait(1200);
    QVERIFY2(client.published == (QList<int>{ 0 }),
             "a forbidden rejection armed a retry");
    // Still counted: the rejection rate is the diagnostic.
    QCOMPARE(presence.publishRejections(), 1);
}

void PresenceManagerTest::syncingEdgePublishesAndPolls()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);

    QVERIFY(settings.sharePresence()); // default ON
    QCOMPARE(client.published.size(), 0); // nothing before the session is live
    goSyncing(client);
    QCOMPARE(client.published.size(), 1);
    QCOMPARE(client.published.first(), 0); // online

    // Staying in Syncing does not publish.
    goSyncing(client);
    QCOMPARE(client.published.size(), 1);

    // A reconnect republishes, but not inside the rate window: the server still
    // holds our state then, and a quick duplicate PUT is what `rc_presence`
    // refuses.
    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QCOMPARE(client.published.size(), 1);

    // Past the window the reconnect republishes, since the server's copy may
    // have expired.
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
    // The debounced burst asks for the first unknowns, capped at one batch.
    QTRY_COMPARE(client.requests.size(), 1);
    QCOMPARE(client.requests.first().userIds.size(), 40);

    // Two scheduled rounds (driven via the Syncing edge) cover all 45 watched
    // users.
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
    QCOMPARE(client.published.last(), 2); // one final offline

    // While disabled, a reconnect publishes nothing.
    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QCOMPARE(client.published.size(), 2);

    // Re-enabling resumes publication immediately.
    settings.setSharePresence(true);
    QCOMPARE(client.published.size(), 3);
    QCOMPARE(client.published.last(), 0);
}

void PresenceManagerTest::pendingFinalOfflineFlushesOnSyncEdge()
{
    // Sharing disabled while the session is not live: the final offline is
    // owed and flushes on the Syncing edge.
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);

    settings.setSharePresence(false);
    QCOMPARE(client.published.size(), 0); // nothing while not syncing

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 2 })); // the owed offline, only

    // Owed once: a reconnect publishes nothing further.
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

// The member profile popover shows "Presence unavailable" only when the client
// actually knows something; these cases separate "unknown" (render nothing)
// from "the server or backend will not answer" (say so once).

void PresenceManagerTest::unknownPresenceIsNotUnavailable()
{
    // Unknown is the default (no answer yet, a transient failure, one
    // forbidden user); none of those reaches "Presence unavailable".
    FakePresenceClient client;
    PresenceManager presence;
    QSignalSpy unavailableSpy(&presence, &PresenceManager::unavailableChanged);
    presence.setClient(&client);
    // Attaching a client answers the capability question once.
    QCOMPARE(unavailableSpy.count(), 1);
    QVERIFY(presence.supported());
    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());

    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);

    // Dispatched, nothing back yet.
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
    QVERIFY(presence.infoFor(QStringLiteral("@alice:example.org")).isEmpty());
    QVERIFY(!presence.unavailable());

    // A network error says nothing about whether the server offers presence.
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@alice:example.org"),
                    QStringLiteral("network")) });
    QVERIFY(!presence.unavailable());

    // Nor does one user's 403, which deliberately does not latch.
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
    // No client is not a finding either; deriving the flag from !supported()
    // would show "Presence unavailable" on the login screen.
    PresenceManager presence;
    QVERIFY(!presence.supported());
    QVERIFY(!presence.active());
    QVERIFY(!presence.unavailable());
}

void PresenceManagerTest::unsupportedBackendReportsUnavailable()
{
    // The first honest disclosure: this backend cannot do presence at all.
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
    // The second: this session's server refused presence for every user.
    // Session-scoped, so signing out retracts it.
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
    QVERIFY(!presence.unavailable()); // one batch is not yet a finding

    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   bothForbidden);
    QVERIFY(presence.unavailable());
    QCOMPARE(unavailableSpy.count(), 1);
    // The capability is not the latch: publishing may still work, so the
    // Settings card stays.
    QVERIFY(presence.supported());

    // A latched session accepts no new polling; a fresh watch does not
    // restart it.
    presence.watch(QStringLiteral("@carol:example.org"));
    QTest::qWait(500); // outlives the burst debounce
    QCOMPARE(client.requests.size(), 2);

    Q_EMIT client.loggedOut();
    QVERIFY(!presence.unavailable());
    QCOMPARE(unavailableSpy.count(), 2);
}

void PresenceManagerTest::forbiddenBatchBelowMinimumNeitherAdvancesNorResetsTheLatch()
{
    // A batch too small to be evidence neither advances nor resets the
    // streak; `latchEligible ? ++streak : streak = 0` would let one
    // interleaved single-user 403 mask a presence-disabled server.
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

    // Streak 1.
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   broadForbidden);
    QVERIFY(presence.active());

    // A one-user refusal in between neither advances nor resets.
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@carol:example.org"),
                    QStringLiteral("forbidden")) });
    QVERIFY(presence.active());

    // Streak 2: two broad batches refused in a row.
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 3);
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   broadForbidden);
    QVERIFY(!presence.active());
    QVERIFY(presence.unavailable());
}

void PresenceManagerTest::repeatedForbiddenEntriesForOneUserNeverLatch()
{
    // The minimum counts distinct user ids, not entries: one user's repeated
    // 403 in a batch (2 entries, 1 user) is not a broad refusal.
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
    // A server answering 200 with offline for everyone refuses nothing: those
    // dots are honest. Only refusals feed the latch.
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
    // forbidden and not_found both mean "no presence for this user": the last
    // known dot is cleared, although a single 403 never latches.
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
    // Clearing one user is not a statement about the server.
    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());
}

void PresenceManagerTest::watchIsRefCountedPerHolder()
{
    // Watches are ref-counted: with two surfaces showing a user, the first
    // unwatch must not stop polling for the one still on screen.
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@alice:example.org")); // second holder
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    // One request entry per user, not per holder.
    QCOMPARE(client.requests.first().userIds.size(), 2);

    presence.unwatch(QStringLiteral("@alice:example.org"));
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    QVERIFY(client.requests.last().userIds.contains(
        QStringLiteral("@alice:example.org")));

    presence.unwatch(QStringLiteral("@alice:example.org"));
    presence.unwatch(QStringLiteral("@bob:example.org"));
    // An unmatched unwatch is a no-op, never a negative count.
    presence.unwatch(QStringLiteral("@alice:example.org"));
    presence.unwatch(QStringLiteral("@nobody:example.org"));
    reconnect(client);
    QTest::qWait(50);
    QCOMPARE(client.requests.size(), 2);
}

void PresenceManagerTest::watchesQueuedBeforeSyncingWaitForTheSyncingEdge()
{
    // A watch registered before the session is live does not poll; the queued
    // burst is discarded, and the Syncing edge re-polls the whole set.
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@bob:example.org"));
    QTest::qWait(500); // outlives kBurstDelayMs
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
    // Only the edge into Syncing polls; repeated Syncing status callbacks do
    // not each start a round.
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

    // A real reconnect is an edge and refreshes every watched dot without
    // waiting for the 30 s round.
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    QCOMPARE(client.requests.last().userIds,
             (QStringList{ QStringLiteral("@alice:example.org") }));
}

void PresenceManagerTest::replayedAnswerForTheSameRoundIsDropped()
{
    // Op ids are single-use: a re-delivered batch is older than the cache and
    // would roll a dot backwards.
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
    // A round dispatched for the previous account may still be in flight when
    // the session ends; its answer never reaches the next session's cache.
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
    // Account switching via setClient ends the session as completely as
    // loggedOut, so the previous watch list is never polled on the new
    // homeserver.
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
    // PresenceDot re-registers on this edge; without it, dots would hold
    // watches the manager no longer has.
    QCOMPARE(epochs.count(), 1);
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());

    goSyncing(second);
    QTest::qWait(500); // outlives the burst debounce too
    QCOMPARE(second.requests.size(), 0);
    QCOMPARE(first.requests.size(), 1);
}

void PresenceManagerTest::presenceIsCompiledInWithNoPlatformConditional()
{
    // Presence is not platform-gated: the Rust module and the C++ policy owner
    // build everywhere. A source scan, since this suite does not link the Rust
    // backend.
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

    // The Rust module, with nothing gating it.
    const QStringList libLines =
        readText(root + QStringLiteral("/rust/src/lib.rs"))
            .split(QLatin1Char('\n'));
    const int modLine = libLines.indexOf(QStringLiteral("mod presence;"));
    QVERIFY2(modLine > 0, "rust/src/lib.rs must declare `mod presence;`");
    QVERIFY2(!libLines.at(modLine - 1).trimmed().startsWith(
                 QStringLiteral("#[")),
             "no attribute may gate the presence module");

    // The C++ policy owner is an unconditional source of the app.
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
    // Before the session is live nothing has been published: unknown, which
    // renders no indicator.
    QCOMPARE(presence.stateFor(me), QString());

    goSyncing(client);
    QCOMPARE(client.published.size(), 1);
    QCOMPARE(presence.stateFor(me), QStringLiteral("online"));
    const QVariantMap info = presence.infoFor(me);
    QCOMPARE(info.value(QStringLiteral("state")).toString(),
             QStringLiteral("online"));
    QCOMPARE(info.value(QStringLiteral("lastActiveAgoMs")).toLongLong(), 0LL);

    // A homeserver with presence off answers 200 "offline" for everyone,
    // including the local user; that must not override what this client
    // knows about itself.
    presence.watch(me);
    QTRY_VERIFY(!client.requests.isEmpty());
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { okEntry(me, QStringLiteral("offline")) });
    QCOMPARE(presence.stateFor(me), QStringLiteral("online"));

    // With sharing off, the server's answer is what everyone else sees, so the
    // override steps aside.
    settings.setSharePresence(false);
    QCOMPARE(presence.stateFor(me), QStringLiteral("offline"));

    // Other users are never answered from the local publication.
    QCOMPARE(presence.stateFor(QStringLiteral("@alice:example.org")),
             QString());
}

// A presence-disabled homeserver answers "offline" for everyone (never a
// refusal, so the latch never fires). A typing notification contradicts that
// offline, and the claim is withdrawn to unknown (renders nothing), never
// replaced with a fabricated "online".
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

    // Withdrawn, not overwritten and not promoted.
    QCOMPARE(presence.stateFor(alice), QString());
    QVERIFY2(presence.stateFor(alice) != QStringLiteral("online"),
             "typing proves activity, never a presence state");
    // infoFor, which cards format from, is empty too (unknown).
    QVERIFY(presence.infoFor(alice).isEmpty());
    // ...and surfaces are told, so a drawn dot repaints.
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

    // A server saying "online" is not contradicted.
    QCOMPARE(presence.stateFor(alice), QStringLiteral("online"));
    // "unavailable" is a soft, server-side idle state; typing while away is
    // not a contradiction.
    QCOMPARE(presence.stateFor(bob), QStringLiteral("unavailable"));
    // A user with no cached answer stays unknown; typing invents nothing.
    QCOMPARE(presence.stateFor(carol), QString());

    // The local user is answered from this client's own publication, so its
    // typing changes nothing.
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   { okEntry(client.self,
                                             QStringLiteral("offline")) });
    presence.noteTyping(client.self);
    QCOMPARE(presence.stateFor(client.self), QStringLiteral("online"));
}

// Typing evidence expires and announces it: applyBatch only bumps the revision
// when a polled value changes, and on a presence-disabled server the cached
// value stays "offline", so nothing else would repaint the dot.
void PresenceManagerTest::typingEvidenceExpiresAndTheDotComesBackOnItsOwn()
{
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);
    // The real window is 35 s; shortened for the test.
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

    // Typing also requests a poll, so on a healthy server the withheld dot is
    // only a brief gap.
    presence.noteTyping(alice);
    QTRY_VERIFY(client.requests.size() > before);
    QVERIFY(client.requests.last().userIds.contains(alice));

    // Typing evidence names contacts, so it is cleared with the session like
    // the watched set.
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
