// v0.7.x Matrix presence: PresenceManager policy tests.
//
// The manager owns the entire client-side presence policy (Sliding Sync
// delivers no presence events, so everything rests on this polling loop):
// watch-set bookkeeping, batch application, honest unknown-vs-offline
// handling, the disabled-server latch, session clearing, and own-presence
// publication gating. These tests drive it against a fake client — network
// behavior and real homeserver semantics are live-validation, not this.

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
    void requestPresence(const QStringList &userIds, quint64 opId) override
    {
        requests.append({ userIds, opId });
    }
    void publishPresence(int state) override { published.append(state); }

    bool supports = true;
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

} // namespace

class PresenceManagerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void watchTriggersOneDebouncedBurst();
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
    void backgroundGraceKeepsOnlineUntilIdleDwell();
    void batchRotationCoversWatchedSetBeyondCap();
    void disablingShareSettingPublishesOfflineOnce();
    void pendingFinalOfflineFlushesOnSyncEdge();
    void unsupportedBackendStaysInactive();

private:
    // Drives the manager to a live session and returns the fake's baseline
    // request count (the edge may or may not have polled, depending on the
    // watched set).
    void goSyncing(FakePresenceClient &client)
    {
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
    // The reported age is the server age advanced by local elapsed time —
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
    // Review H1 contract, manager level: a "server sent none" age (-1,
    // the decode default for an absent key) must stay -1 — never advanced
    // by local elapsed time into a fabricated "active just now".
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

    // A network blip on the next round must not erase the known state.
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
    QVERIFY(presence.supported()); // capability is not the latch (M1)

    // A latched manager stops polling entirely.
    const int before = client.requests.size();
    Q_EMIT client.connectionStateChanged(MatrixClient::Error);
    goSyncing(client);
    QTest::qWait(50);
    QCOMPARE(client.requests.size(), before);
}

void PresenceManagerTest::singleUserForbiddenNeverLatches()
{
    // One user's 403 (a federation edge, an invited-not-joined member)
    // must not blind presence for the whole session (review L1).
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
    // The latch is per session: the next account may be on a server that
    // supports presence.
    QVERIFY(presence.active());
}

void PresenceManagerTest::loggedOutDropsWatchedSet()
{
    // Review M2: the previous account's watch list must never be polled
    // against the next account's homeserver.
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

    // The next session's Syncing edge polls nothing: the watched set died
    // with the previous session.
    goSyncing(client);
    QTest::qWait(500); // outlives the burst debounce too
    QCOMPARE(client.requests.size(), 1);
}

void PresenceManagerTest::backgroundGraceKeepsOnlineUntilIdleDwell()
{
    // Review H2: the idle clock measures CONTINUOUS background dwell.
    // The regression measured time since focus was GAINED, so any session
    // focused longer than the threshold published Away the instant the
    // user switched windows.
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
    // Hold focus well past the threshold: staying active must never decay
    // toward Away, whatever the keep-alive publishes.
    QTest::qWait(400);
    QVERIFY(!client.published.contains(1));

    // Losing focus NOW must not publish Away immediately — the dwell
    // clock starts at this moment (the old code compared against focus
    // GAIN, ~600ms ago > 200ms, and failed exactly here).
    presence.setApplicationActive(false);
    QVERIFY(!client.published.contains(1));

    // After the background dwell exceeds the threshold, the keep-alive
    // honestly reports Away.
    QTRY_VERIFY(client.published.contains(1));
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

    // Re-entering Syncing (reconnect) publishes again; staying in Syncing
    // does not.
    goSyncing(client);
    QCOMPARE(client.published.size(), 1);
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

    // Two scheduled rounds (driven via the Syncing edge) rotate through
    // everyone: together they must cover all 45 watched users.
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
    // Review M3: sharing disabled while the session is not live — the
    // promised final offline is owed, and flushes on the Syncing edge
    // instead of being silently skipped.
    FakePresenceClient client;
    SettingsManager settings;
    PresenceManager presence;
    presence.setSettings(&settings);
    presence.setClient(&client);

    settings.setSharePresence(false);
    QCOMPARE(client.published.size(), 0); // nothing while not syncing

    goSyncing(client);
    QCOMPARE(client.published, (QList<int>{ 2 })); // the owed offline, only

    // And it is owed ONCE: a reconnect publishes nothing further.
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

QTEST_MAIN(PresenceManagerTest)
#include "PresenceManagerTest.moc"
