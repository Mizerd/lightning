// v0.7.x Matrix presence: PresenceManager policy tests.
//
// The manager owns the entire client-side presence policy (Sliding Sync
// delivers no presence events, so everything rests on this polling loop):
// watch-set bookkeeping, batch application, honest unknown-vs-offline
// handling, the disabled-server latch, session clearing, and own-presence
// publication gating. These tests drive it against a fake client — network
// behavior and real homeserver semantics are live-validation, not this.

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

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// Both build trees live inside the source tree, so walking up from the test
// binary locates the repository without a compile definition (the
// UpdateManagerStateTest idiom). Returns an empty string when the scan
// cannot find the tree, which the caller treats as a failure rather than a
// pass.
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
    void backgroundGraceKeepsOnlineUntilIdleDwell();
    void batchRotationCoversWatchedSetBeyondCap();
    void disablingShareSettingPublishesOfflineOnce();
    void pendingFinalOfflineFlushesOnSyncEdge();
    void unsupportedBackendStaysInactive();

    // 2026-08-20 correctness round (contract C10). The `unavailable`
    // property the profile popover reads, the latch's distinct-user
    // minimum, watch ref-counting, connection-state gating, cross-session
    // isolation, and the platform-independence build contract.
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

private:
    // Drives the manager to a live session and returns the fake's baseline
    // request count (the edge may or may not have polled, depending on the
    // watched set).
    void goSyncing(FakePresenceClient &client)
    {
        Q_EMIT client.connectionStateChanged(MatrixClient::Syncing);
    }

    // Ask for one more authoritative round. The scheduled timer is 30 s, so
    // a reconnect EDGE is how a test drives the next round without waiting
    // for it; callers assert the resulting request count themselves rather
    // than have this helper swallow a failure.
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

    // A message from a watched sender is only a freshness hint: it queues
    // another authoritative server read instead of fabricating online state.
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

// --- 2026-08-20 correctness round (contract C10) -------------------------
//
// The round's UI change is one line of prose in the member profile popover,
// and the whole risk is that it appears when the client does not actually
// KNOW anything. So most of what follows discriminates "unknown" (render
// nothing) from "the server or backend will not answer" (say so once).

void PresenceManagerTest::unknownPresenceIsNotUnavailable()
{
    // Unknown is the default state of the world: nobody has answered yet, a
    // lookup failed transiently, one user is forbidden. None of those is a
    // finding about the server, and none may reach the popover's
    // "Presence unavailable" line.
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

    // A network blip says nothing about whether the server offers presence.
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@alice:example.org"),
                    QStringLiteral("network")) });
    QVERIFY(!presence.unavailable());

    // Neither does ONE user's 403 — that is a federation or
    // invited-not-joined edge, and it deliberately does not latch.
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
    // Having no client is not a finding either: nothing has been asked of
    // any server. An implementation deriving the flag from !supported()
    // would claim "Presence unavailable" on the login screen.
    PresenceManager presence;
    QVERIFY(!presence.supported());
    QVERIFY(!presence.active());
    QVERIFY(!presence.unavailable());
}

void PresenceManagerTest::unsupportedBackendReportsUnavailable()
{
    // The first of exactly two honest disclosures: this backend cannot do
    // presence at all, which the client knows without asking anyone.
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
    // The second disclosure: this session's server refused presence for
    // every user. It is SESSION scoped — the next account may be on a
    // server that answers — so signing out must retract it.
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
    // The capability is not the latch (review M1): publication may still
    // run, so the Settings card must not disappear.
    QVERIFY(presence.supported());

    // A latched session accepts no new polling at all — a fresh watch must
    // not restart it.
    presence.watch(QStringLiteral("@carol:example.org"));
    QTest::qWait(500); // outlives the burst debounce
    QCOMPARE(client.requests.size(), 2);

    Q_EMIT client.loggedOut();
    QVERIFY(!presence.unavailable());
    QCOMPARE(unavailableSpy.count(), 2);
}

void PresenceManagerTest::forbiddenBatchBelowMinimumNeitherAdvancesNorResetsTheLatch()
{
    // The subtle half of review L1. A batch too small to be evidence is
    // evidence for NEITHER side: it must not advance the streak, and it
    // must not throw the streak away. The tempting shape —
    // `latchEligible ? ++streak : streak = 0` — passes every other latch
    // test in this file and lets one interleaved single-user 403 mask a
    // genuinely presence-disabled server for the whole session.
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

    // A one-user refusal in between: neither advances nor resets.
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    Q_EMIT client.presenceReceived(
        client.requests.last().opId,
        { failEntry(QStringLiteral("@carol:example.org"),
                    QStringLiteral("forbidden")) });
    QVERIFY(presence.active());

    // Streak 2: the server has now refused two broad batches in a row.
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 3);
    Q_EMIT client.presenceReceived(client.requests.last().opId,
                                   broadForbidden);
    QVERIFY(!presence.active());
    QVERIFY(presence.unavailable());
}

void PresenceManagerTest::repeatedForbiddenEntriesForOneUserNeverLatch()
{
    // The minimum counts DISTINCT user ids, not entries. One user's
    // repeated 403 inside a single batch must not read as a broad refusal —
    // entries.size() is 2 here and the distinct count is 1.
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
    // A server answering 200 with offline for everyone is not refusing
    // anything: those grey dots are honest, and the popover must keep
    // showing the real state rather than "Presence unavailable". Only
    // refusals feed the latch.
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
    // forbidden and not_found are both authoritative "no presence for this
    // user": the last known dot has to go, even though a single 403 never
    // latches. Treating forbidden as transient would leave a stale online
    // dot on a user the server has stopped answering for.
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
    // Erasing one user is not a statement about the server.
    QVERIFY(presence.active());
    QVERIFY(!presence.unavailable());
}

void PresenceManagerTest::watchIsRefCountedPerHolder()
{
    // Two surfaces routinely show the same user at once (a DM row and an
    // open profile popover). The first holder going away must not stop
    // polling for the one still on screen — an unwatch that erased the
    // entry outright would blank the dot under the open popover.
    FakePresenceClient client;
    PresenceManager presence;
    presence.setClient(&client);
    goSyncing(client);
    presence.watch(QStringLiteral("@alice:example.org"));
    presence.watch(QStringLiteral("@alice:example.org")); // second holder
    presence.watch(QStringLiteral("@bob:example.org"));
    QTRY_COMPARE(client.requests.size(), 1);
    // One request entry per USER, not per holder.
    QCOMPARE(client.requests.first().userIds.size(), 2);

    presence.unwatch(QStringLiteral("@alice:example.org"));
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    QVERIFY(client.requests.last().userIds.contains(
        QStringLiteral("@alice:example.org")));

    presence.unwatch(QStringLiteral("@alice:example.org"));
    presence.unwatch(QStringLiteral("@bob:example.org"));
    // An unwatch with no matching watch is a no-op — never a negative count
    // that a later watch would have to climb back out of.
    presence.unwatch(QStringLiteral("@alice:example.org"));
    presence.unwatch(QStringLiteral("@nobody:example.org"));
    reconnect(client);
    QTest::qWait(50);
    QCOMPARE(client.requests.size(), 2);
}

void PresenceManagerTest::watchesQueuedBeforeSyncingWaitForTheSyncingEdge()
{
    // Connection gating. A watch registered before the session is live must
    // not poll a server we are not synced with; the queued burst is
    // DISCARDED rather than deferred, and the Syncing edge is the recovery
    // that re-polls the whole watched set.
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
    // Only the EDGE into Syncing polls. The sync loop reports its state
    // more than once, and turning every status callback into a round of
    // GETs is how a bounded poller stops being bounded.
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

    // A real reconnect is an edge, and it refreshes every watched dot
    // without waiting out the 30 s scheduled round.
    reconnect(client);
    QTRY_COMPARE(client.requests.size(), 2);
    QCOMPARE(client.requests.last().userIds,
             (QStringList{ QStringLiteral("@alice:example.org") }));
}

void PresenceManagerTest::replayedAnswerForTheSameRoundIsDropped()
{
    // Op ids are single-use: the answer is consumed, not merely matched. A
    // re-delivered batch is by definition older than what is already
    // cached, so applying it would roll a dot backwards.
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
    // Generation isolation, presence edition (review M2). A round dispatched
    // for the previous account can still be in flight when the session ends;
    // its answer must never populate the next session's cache.
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
    // Account switching goes through setClient, not loggedOut. It must end
    // the session just as completely: polling the previous account's watch
    // list against the next account's homeserver would disclose who that
    // account was looking at.
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
    // PresenceDot re-registers on this edge; without the bump the surviving
    // dots would hold watches the manager no longer has.
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
    // Contract C10's build assertion. The reported "no presence on macOS"
    // was hypothesised to be a packaging omission; reading refuted that, and
    // this pins the refutation so a future platform guard has to be a
    // deliberate decision rather than a silent regression that presents to
    // the user as "the dots are gone". A source scan, because the presence
    // suite deliberately does not link the Rust backend.
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

    // The Rust module itself, and nothing gating it.
    const QStringList libLines =
        readText(root + QStringLiteral("/rust/src/lib.rs"))
            .split(QLatin1Char('\n'));
    const int modLine = libLines.indexOf(QStringLiteral("mod presence;"));
    QVERIFY2(modLine > 0, "rust/src/lib.rs must declare `mod presence;`");
    QVERIFY2(!libLines.at(modLine - 1).trimmed().startsWith(
                 QStringLiteral("#[")),
             "no attribute may gate the presence module");

    // And the C++ policy owner is an unconditional source of the app.
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

QTEST_MAIN(PresenceManagerTest)
#include "PresenceManagerTest.moc"
