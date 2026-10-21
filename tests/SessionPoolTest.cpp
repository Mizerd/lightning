// Live accounts Phase 1: the pool of background accounts (design §5, §6,
// §9.2, §10.1; contract §5.2).
//
//  * nothing runs before start(); start() is the only labs_enable;
//  * 200 saved accounts, 30 pin requests: four pins, zero keyring reads for
//    everything that is not a candidate;
//  * the gate, one catch-up at a time (first start AND resume, foreground
//    first), backoff and its reset when the foreground reaches the server;
//  * revoked, precheck-refused, legacy-keyring and store-busy accounts are
//    never started;
//  * the click path: retire, hand back, await the latch; a Starting host is
//    retired only after its login result (R1-C3); the C2 sequence;
//  * removal signs out, writes nothing back, awaits that store's latch;
//  * revocation: classification, proof, revoked mark, retire, signal;
//  * a failed token write pauses sync and the send queue and retries.
// Plus the scheduler's pure rules (backoff table, jitter, probe, tracker).
//
// UNRUN when written (WP agents do not build); the lead runs it offscreen.
#include "accounts/BackgroundDrainer.h"
#include "accounts/BackgroundSessionHost.h"
#include "accounts/HostRetirementPool.h"
#include "accounts/KeyringWorker.h"
#include "accounts/RetirementLatches.h"
#include "accounts/RevocationProofs.h"
#include "accounts/SessionPool.h"
#include "accounts/SessionScheduler.h"
#include "accounts/SessionTokenCustody.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "models/AccountActivityModel.h"
#include "storage/AppDataPaths.h"
#include "support/CountingSecretStore.h"
#include "support/FakeRustBridge.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using lightning::accounts::BackgroundSessionHost;
using lightning::accounts::CatchUpTracker;
using lightning::accounts::HostSignInRetirements;
using lightning::accounts::KeyringWorker;
using lightning::accounts::SessionTokenPair;
namespace custody = lightning::accounts::custody;
using lightning::accounts::SessionPool;
using lightning::accounts::SessionScheduler;
using lightning::accounts::testing::CountingSecretStore;
using lightning::accounts::testing::FakeHandle;
using lightning::accounts::testing::FakeRustBridge;
using LiveState = AccountActivityModel::LiveState;

namespace {

class ForwardingSecretStore final : public SecretStore
{
public:
    explicit ForwardingSecretStore(SecretStore *target) : m_target(target) {}
    bool isSecure() const override { return m_target->isSecure(); }
    bool isAvailable() const override { return m_target->isAvailable(); }
    QString backendName() const override { return m_target->backendName(); }
    bool storeSecret(const QString &u, const QString &k, const QString &v) override
    { return m_target->storeSecret(u, k, v); }
    QString readSecret(const QString &u, const QString &k) const override
    { return m_target->readSecret(u, k); }
    bool deleteSecret(const QString &u, const QString &k) override
    { return m_target->deleteSecret(u, k); }
    bool clearAccountSecrets(const QString &u) override { return m_target->clearAccountSecrets(u); }
    QString lastError() const override { return m_target->lastError(); }
    bool lastReadFailed() const override { return m_target->lastReadFailed(); }

private:
    SecretStore *m_target;
};

QString uidOf(int i, const QString &server = QStringLiteral("example.org"))
{
    return QStringLiteral("@u%1:%2").arg(i).arg(server);
}

const QString kForeground = QStringLiteral("@fg:example.org");

} // namespace

class SessionPoolTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    // scheduler rules (pure)
    void backoffIsOneFiveFifteenSixtyMinutesPlusMinusTwentyPercent();
    void theLagProbeGatesAndDetectsAResume();
    void catchUpCompletesOnRunningAfterSyncWithALowRateOrAfterSixtySeconds();
    void fdHeadroomComesFromConfiguredMaxima();
    void homeserverRulesPauseAndReset();
    // pool
    void nothingRunsBeforeStartAndStartEnablesLabsOnce();
    void twoHundredAccountsThirtyPinsFourRunAndNothingElseIsRead();
    void nothingStartsBeforeTheGate();
    void oneCatchUpAtATime();
    void aResumeRestartsHostsOneAtATimeAfterTheForeground();
    void aTransientFailureBacksOffUntilTheForegroundReachesTheServer();
    void accountsThatMustNotRunNeverStart();
    void releaseRetiresHandsBackThenFreesTheStore();
    void aStartingHostIsRetiredOnlyAfterItsLoginResult();
    void anUnconfirmedCloseKeepsTheStoreBusy();
    void aStoreTheForegroundStillHoldsIsWaitedForWithoutKeyringReads();
    void removalSignsOutWritesNothingBackAndAwaitsThatStore();
    void aRevokedHostRecordsTheProofAndIsNeverRestarted();
    void aFailedTokenWritePausesSyncAndTheSendQueueAndRetries();
    // review round 1
    void aRetireAfterALostConnectionWaitsForTheStop();
    void unpinThenRemoveFinishesTheHandbackAndWritesNothing();
    void aLateRevocationProbeForARetiredHostChangesNothing();
    void openingAnAccountInTheForegroundClearsItsRefusals();
    void anUnconfirmedForegroundCloseIsStoreBusy();
    void aDelegatedStoreSlugWaitsForTheForegroundInsteadOfStoreBusy();
    void aSignOutWithNoSessionIsNotReportedAsDone();
    // review round 2
    void quitRetiresEveryHostInParallel();
    void aRemovedAccountSignedInAgainMayRunInTheBackgroundAgain();
    void aLiveWriteCancelledOutsideARemovalPausesInsteadOfVanishing();
    void anAccountRemovedWhileNotLiveShowsItsRowWhenAdmitted();

    // liveacc port onto 24a485c5 (2026-10-20)
    void aBackgroundRotationIsAnsweredOnlyOnceTheKeyringHoldsIt();
    void anAbandonedReleaseLetsThePinRunAgain();
    // §18 round 1, MUST 1
    void aRefusedHandbackIsParkedForTheForegroundsRead();
    void aTimedOutHandbackKeepsTheStoreUntilTheWriteReturns();
    // §18 round 1, follow-ups
    void aRuntimeOffWaitsForAHostStillRestoring();
    void aForegroundOpenDropsABackgroundKeyringReadInFlight();
    void nothingPinnedOrHostedStopsTheTimers();
    // §18 round 2
    void aParkedPairIsWrittenOverTheHostsOwnStrandedLiveWriteBeforeItStartsAgain();
    void aQuitLeavesAParkedPairBehindAWriteStillInFlightAndStaysInBudget();
    // §18 round 3
    void aSwitchWaitsForAParkedPairStillBeingWritten();
    // §18 round 4
    void aRemovalWaitsForAParkedPairsWriteAndDiscardsThePair();

private:
    void saveAccount(const QString &uid, const QString &token = QString());
    std::unique_ptr<SessionPool> makePool();
    FakeHandle *handleFor(const QString &uid) const;
    QString storePathOf(const QString &uid) const;
    // A host from create to Running with its catch-up complete.
    void bringToRunning(SessionPool &pool, const QString &uid);
    void drain(SessionPool &pool) { pool.drainerForTest()->tick(); }
    // start(), then the real-time lag probe off (the clock here is scripted:
    // a jump would read as a suspend) and the fd limit out of the picture.
    void startPool(SessionPool &pool)
    {
        pool.start();
        pool.schedulerForTest()->stopProbe();
        pool.schedulerForTest()->setFdSoftLimitForTest(0);
    }

    QTemporaryDir m_config;
    CountingSecretStore m_store;
    std::unique_ptr<SettingsManager> m_settings;
    std::unique_ptr<AccountManager> m_accounts;
    std::unique_ptr<AccountActivityModel> m_activity;
    std::unique_ptr<lightning::accounts::RevocationProofs> m_proofs;
    std::unique_ptr<lightning::accounts::StoreOwnershipRegistry> m_registry;
    std::unique_ptr<FakeRustBridge> m_fake;
    qint64 m_now = 0;
    // Time the machine spent asleep: the suspend-aware clock is m_now plus
    // this, and the monotonic one (m_now) does not see it (review M3).
    qint64 m_asleep = 0;
};

void SessionPoolTest::initTestCase()
{
    QVERIFY(m_config.isValid());
    qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
    qputenv("XDG_DATA_HOME", m_config.filePath(QStringLiteral("data")).toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("session-pool-test"));
}

void SessionPoolTest::init()
{
    {
        QSettings wipe;
        wipe.clear();
        wipe.sync();
    }
    m_store.setFailReads(false);
    m_store.setFailWrites(false);
    m_store.setReadLatencyMs(0);
    m_store.setWriteLatencyMs(0);
    m_store.setShared(false);
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(&m_store);
    m_accounts = std::make_unique<AccountManager>(m_settings.get());
    m_activity = std::make_unique<AccountActivityModel>();
    m_proofs = std::make_unique<lightning::accounts::RevocationProofs>();
    m_registry = std::make_unique<lightning::accounts::StoreOwnershipRegistry>();
    m_fake = std::make_unique<FakeRustBridge>();
    HostSignInRetirements::process().clearForTest();
    m_now = 0;
    m_asleep = 0;
    saveAccount(kForeground);
    m_store.resetCounts();
}

void SessionPoolTest::cleanup()
{
    m_fake.reset();
    m_registry.reset();
    m_proofs.reset();
    m_activity.reset();
    m_accounts.reset();
    m_settings.reset();
}

void SessionPoolTest::saveAccount(const QString &uid, const QString &token)
{
    const QString server = uid.mid(uid.indexOf(QLatin1Char(':')) + 1);
    m_settings->saveSession(QStringLiteral("https://") + server, uid,
                            QStringLiteral("DEV-") + uid.mid(1, uid.indexOf(QLatin1Char(':')) - 1),
                            token.isEmpty() ? QStringLiteral("token-of-") + uid : token,
                            QStringLiteral("refresh-of-") + uid);
}

std::unique_ptr<SessionPool> SessionPoolTest::makePool()
{
    SessionPool::Deps deps;
    deps.settings = m_settings.get();
    deps.accounts = m_accounts.get();
    deps.activity = m_activity.get();
    deps.proofs = m_proofs.get();
    deps.registry = m_registry.get();
    deps.api = m_fake.get();
    deps.guiSecretStore = &m_store;
    deps.secretStoreFactory = [this]() -> std::unique_ptr<SecretStore> {
        return std::make_unique<ForwardingSecretStore>(&m_store);
    };
    deps.appStartMs = 1760000000000;
    deps.monotonicMs = [this] { return m_now; };
    deps.suspendAwareMs = [this] { return m_now + m_asleep; };
    auto pool = std::make_unique<SessionPool>(deps);
    pool->setForegroundUserId(kForeground);
    return pool;
}

QString SessionPoolTest::storePathOf(const QString &uid) const
{
    matrix::app_data::AccountIdentity identity;
    return m_settings->resolveSavedIdentity(uid, &identity) ? identity.rustStorePath : QString();
}

FakeHandle *SessionPoolTest::handleFor(const QString &uid) const
{
    return m_fake->handleFor(storePathOf(uid));
}

void SessionPoolTest::bringToRunning(SessionPool &pool, const QString &uid)
{
    QTRY_VERIFY_WITH_TIMEOUT(handleFor(uid) != nullptr, 3000);
    FakeHandle *h = handleFor(uid);
    m_fake->setSticky(h, QStringLiteral("initial_sync_done"), QJsonObject{});
    drain(pool);   // login_ok (auto) and the initial sync
    QCOMPARE(pool.liveState(uid), AccountActivityModel::Running);
    m_fake->setSticky(h, QStringLiteral("room_list_sync_state"),
                      QJsonObject{{QStringLiteral("state"), QStringLiteral("running")}});
    drain(pool);
    m_now += CatchUpTracker::kRateWindowMs + 100;
    drain(pool);   // a quiet 3 s: catch-up complete, the semaphore released
    m_now += 1100;  // past the U(0, 1 s) cooldown
    QVERIFY(pool.schedulerForTest()->catchUpHolder().isEmpty());
}

// ---- scheduler rules -------------------------------------------------------------------

void SessionPoolTest::backoffIsOneFiveFifteenSixtyMinutesPlusMinusTwentyPercent()
{
    const qint64 minute = 60 * 1000;
    QCOMPARE(SessionScheduler::backoffDelayMs(1, 0.5), 1 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(2, 0.5), 5 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(3, 0.5), 15 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(4, 0.5), 60 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(9, 0.5), 60 * minute);
    // +-20 %, to the millisecond (the jitter is computed in floating point).
    QVERIFY(qAbs(SessionScheduler::backoffDelayMs(1, 0.0) - 48 * 1000) <= 1);
    QVERIFY(qAbs(SessionScheduler::backoffDelayMs(1, 1.0) - 72 * 1000) <= 1);
    QVERIFY(qAbs(SessionScheduler::backoffDelayMs(4, 1.0) - 72 * minute) <= 1);
}

void SessionPoolTest::theLagProbeGatesAndDetectsAResume()
{
    qint64 now = 0;
    qint64 asleep = 0;
    SessionScheduler scheduler([&now] { return now; }, [] { return 0.5; });
    scheduler.setSuspendAwareClock([&now, &asleep] { return now + asleep; });
    QSignalSpy resumed(&scheduler, &SessionScheduler::resumeDetected);
    QVERIFY(!scheduler.gateOpen());   // not started
    scheduler.noteStarted();
    scheduler.startProbe();
    scheduler.stopProbe();   // ticks are driven by hand below
    QVERIFY(!scheduler.gateOpen());   // the foreground has not synced, 15 s not passed
    scheduler.noteForegroundInitialSyncDone();
    QVERIFY(scheduler.gateOpen());
    // A tick 300 ms late: not clear for the next 2 s.
    now = 0;
    scheduler.probeTick();
    now += 100;
    scheduler.probeTick();
    now += 400;
    scheduler.probeTick();
    QVERIFY(!scheduler.lagClear());
    QVERIFY(!scheduler.gateOpen());
    for (int i = 0; i < 21; ++i) {
        now += 100;
        scheduler.probeTick();
    }
    QVERIFY(scheduler.lagClear());
    QCOMPARE(resumed.size(), 0);
    // A GUI stall of 11 s moves both clocks alike: lateness, never a resume
    // (review M3: it was one, and it restarted every host's sync).
    now += 11000;
    scheduler.probeTick();
    QCOMPARE(resumed.size(), 0);
    QVERIFY(scheduler.worstLatenessMs() > SessionScheduler::kResumeLagMs);
    QVERIFY(!scheduler.lagClear());
    for (int i = 0; i < 21; ++i) {
        now += 100;
        scheduler.probeTick();
    }
    QVERIFY(scheduler.lagClear());
    QVERIFY(scheduler.catchUpFree());
    // A suspend: the monotonic clock stood still (no lateness at all), the
    // suspend-aware one moved 11 s.
    asleep += 11000;
    now += 100;
    scheduler.probeTick();
    QCOMPARE(resumed.size(), 1);
    QVERIFY(scheduler.lagClear());
    // After a resume the foreground goes first: no catch-up until it synced
    // (or the grace ends).
    QVERIFY(!scheduler.catchUpFree());
    scheduler.noteForegroundInitialSyncDone();
    QVERIFY(scheduler.catchUpFree());

    // The 15 s fallback when the foreground never reports.
    qint64 t2 = 0;
    SessionScheduler late([&t2] { return t2; }, [] { return 0.5; });
    late.noteStarted();
    QVERIFY(!late.gateOpen());
    t2 = SessionScheduler::kGateFallbackMs;
    QVERIFY(late.gateOpen());
}

void SessionPoolTest::catchUpCompletesOnRunningAfterSyncWithALowRateOrAfterSixtySeconds()
{
    CatchUpTracker tracker;
    tracker.begin(0, 5);
    tracker.sample(0, 100);
    tracker.sample(3100, 110);
    // "running" carried over (version unchanged): not yet.
    QVERIFY(!tracker.complete(3100, true, true, 5));
    // initial sync not done: not yet.
    QVERIFY(!tracker.complete(3100, false, true, 6));
    // A fresh running, 10 updates in 3.1 s (< 5/s): complete.
    QVERIFY(tracker.complete(3100, true, true, 6));
    // A busy host (100/s) is not caught up...
    CatchUpTracker busy;
    busy.begin(0, 1);
    busy.sample(0, 0);
    busy.sample(3000, 300);
    QVERIFY(!busy.complete(3000, true, true, 2));
    // ...until the 60 s cap.
    QVERIFY(busy.complete(CatchUpTracker::kCapMs, false, false, 1));
}

void SessionPoolTest::fdHeadroomComesFromConfiguredMaxima()
{
    SessionScheduler scheduler([] { return qint64(0); }, [] { return 0.5; });
    scheduler.setFdSoftLimitForTest(1024);
    // 256 + 56 * (hosts + foreground) <= 70 % of 1024 = 716.8
    QVERIFY(scheduler.fdHeadroomFor(7));    // 256 + 448 = 704
    QVERIFY(!scheduler.fdHeadroomFor(8));   // 760
    scheduler.setFdSoftLimitForTest(0);     // unlimited / not applicable
    QVERIFY(scheduler.fdHeadroomFor(100));
    scheduler.setCap(5);
    QVERIFY(scheduler.underCap(3, true));
    QVERIFY(!scheduler.underCap(4, true));
    QVERIFY(scheduler.underCap(4, false));
}

void SessionPoolTest::homeserverRulesPauseAndReset()
{
    qint64 now = 0;
    SessionScheduler scheduler([&now] { return now; }, [] { return 0.5; });
    const QString hs = QStringLiteral("https://example.org");
    scheduler.noteStarting(hs, QStringLiteral("@a:example.org"));
    QVERIFY(scheduler.homeserverStarting(hs));
    scheduler.noteStartFinished(hs, QStringLiteral("@b:example.org"));   // not the starter
    QVERIFY(scheduler.homeserverStarting(hs));
    scheduler.noteStartFinished(hs, QStringLiteral("@a:example.org"));
    QVERIFY(!scheduler.homeserverStarting(hs));

    scheduler.pauseHomeserver(hs);
    QVERIFY(scheduler.homeserverPaused(hs));
    now = SessionScheduler::kHomeserverPauseMs - 1;
    QVERIFY(scheduler.homeserverPaused(hs));
    now = SessionScheduler::kHomeserverPauseMs;
    QVERIFY(!scheduler.homeserverPaused(hs));

    scheduler.noteFailure(QStringLiteral("@a:example.org"), hs, false);
    QVERIFY(scheduler.inBackoff(QStringLiteral("@a:example.org")));
    scheduler.noteFailure(QStringLiteral("@p:example.org"), hs, true);
    scheduler.noteHomeserverReachable(hs);
    QVERIFY(!scheduler.inBackoff(QStringLiteral("@a:example.org")));
    // A permanent refusal is not a network question: reachability resets nothing.
    QVERIFY(scheduler.permanentlyRefused(QStringLiteral("@p:example.org")));
}

// ---- pool --------------------------------------------------------------------------------

void SessionPoolTest::nothingRunsBeforeStartAndStartEnablesLabsOnce()
{
    saveAccount(uidOf(1));
    QCOMPARE(m_settings->setKeepRunningFor(uidOf(1), true), SettingsManager::KeepRunningResult::Ok);
    auto pool = makePool();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->totalCalls(), 0);
    QCOMPARE(m_store.totalReads(), 0);
    QVERIFY(!pool->drainerForTest()->running());
    startPool(*pool);
    startPool(*pool);
    QCOMPARE(m_fake->calls(QStringLiteral("labsEnable")), 1);
    pool->shutdownAsync([] {});
}

// §6.3: 200 saved accounts cost nothing but their pins.
void SessionPoolTest::twoHundredAccountsThirtyPinsFourRunAndNothingElseIsRead()
{
    for (int i = 0; i < 200; ++i)
        saveAccount(uidOf(i, QStringLiteral("s%1.example").arg(i)));
    m_store.resetCounts();
    auto pool = makePool();
    QStringList pinned;
    int refusedCap = 0;
    for (int i = 0; i < 30; ++i) {
        const QString uid = uidOf(i * 6, QStringLiteral("s%1.example").arg(i * 6));
        const SessionPool::PinResult r = pool->setKeepRunning(uid, true);
        if (r == SessionPool::PinResult::Ok)
            pinned << uid;
        else if (r == SessionPool::PinResult::RefusedCap)
            ++refusedCap;
    }
    QCOMPARE(pinned.size(), SettingsManager::kMaxPinnedAccounts);
    QCOMPARE(refusedCap, 30 - SettingsManager::kMaxPinnedAccounts);
    QCOMPARE(pool->setKeepRunning(QStringLiteral("@nobody:nowhere.example"), true),
             SessionPool::PinResult::RefusedNoRecord);

    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    // Admitted in the settings' saved order (keepRunningUserIds()), not in
    // the order they were pinned: "@u12" sorts before "@u6" (integration).
    const QStringList admissionOrder = m_settings->keepRunningUserIds();
    QCOMPARE(QSet<QString>(admissionOrder.cbegin(), admissionOrder.cend()),
             QSet<QString>(pinned.cbegin(), pinned.cend()));
    for (const QString &uid : admissionOrder) {
        pool->admitNowForTest();
        bringToRunning(*pool, uid);
    }
    QCOMPARE(pool->liveHostCount(), SettingsManager::kMaxPinnedAccounts);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), SettingsManager::kMaxPinnedAccounts);
    // Keyring reads only for the four pins; none for the 196 others.
    const QSet<QString> readUsers = m_store.readUsers();
    QCOMPARE(readUsers, QSet<QString>(pinned.cbegin(), pinned.cend()));
    pool->shutdownAsync([] {});
    QTRY_COMPARE_WITH_TIMEOUT(pool->liveHostCount(), 0, 5000);
}

void SessionPoolTest::nothingStartsBeforeTheGate()
{
    saveAccount(uidOf(1));
    QCOMPARE(m_settings->setKeepRunningFor(uidOf(1), true), SettingsManager::KeepRunningResult::Ok);
    auto pool = makePool();
    startPool(*pool);
    pool->admitNowForTest();
    QTest::qWait(50);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    QCOMPARE(m_store.reads(uidOf(1)), 0);
    // The 15 s fallback opens it without the foreground's initial sync.
    m_now = SessionScheduler::kGateFallbackMs;
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 1, 3000);
    pool->shutdownAsync([] {});
}

void SessionPoolTest::oneCatchUpAtATime()
{
    const QString a = uidOf(1, QStringLiteral("one.example"));
    const QString b = uidOf(2, QStringLiteral("two.example"));
    saveAccount(a);
    saveAccount(b);
    QCOMPARE(m_settings->setKeepRunningFor(a, true), SettingsManager::KeepRunningResult::Ok);
    QCOMPARE(m_settings->setKeepRunningFor(b, true), SettingsManager::KeepRunningResult::Ok);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 1, 3000);
    // A different homeserver, and still: B waits for A's catch-up.
    for (int i = 0; i < 5; ++i) {
        drain(*pool);
        pool->admitNowForTest();
        QCoreApplication::processEvents();
    }
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    const QString first = handleFor(a) ? a : b;
    const QString second = first == a ? b : a;
    bringToRunning(*pool, first);
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 2, 3000);
    QVERIFY(handleFor(second));
    pool->shutdownAsync([] {});
}

void SessionPoolTest::aResumeRestartsHostsOneAtATimeAfterTheForeground()
{
    const QString a = uidOf(1, QStringLiteral("one.example"));
    const QString b = uidOf(2, QStringLiteral("two.example"));
    saveAccount(a);
    saveAccount(b);
    m_settings->setKeepRunningFor(a, true);
    m_settings->setKeepRunningFor(b, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(handleFor(a) || handleFor(b), 3000);
    const QString first = handleFor(a) ? a : b;
    const QString second = first == a ? b : a;
    bringToRunning(*pool, first);
    pool->admitNowForTest();
    bringToRunning(*pool, second);
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 2);

    // A GUI stall of 11 s is not a suspend: nothing is stopped.
    pool->schedulerForTest()->probeTick();
    m_now += 11000;
    pool->schedulerForTest()->probeTick();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("stopSync")), 0);
    // A suspend of 11 s: only the suspend-aware clock moved.
    m_asleep += 11000;
    m_now += 100;
    pool->schedulerForTest()->probeTick();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("stopSync")), 2, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(pool->retirementForTest()->activeJobs(), 0, 3000);
    m_now += SessionScheduler::kLagWindowMs + 100;   // the suspend's own lateness ages out
    // The foreground first: nothing restarts during its grace.
    pool->admitNowForTest();
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 2);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 3);   // ONE host
    pool->admitNowForTest();
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 3);
    // That host catches up; then the other.
    const QString restarted = m_fake->log(handleFor(a)).count(QStringLiteral("startSync")) == 2 ? a : b;
    FakeHandle *h = handleFor(restarted);
    m_fake->setSticky(h, QStringLiteral("room_list_sync_state"),
                      QJsonObject{{QStringLiteral("state"), QStringLiteral("running")},
                                  {QStringLiteral("n"), 2}});
    drain(*pool);
    m_now += CatchUpTracker::kRateWindowMs + 100;
    drain(*pool);
    m_now += 1100;
    pool->admitNowForTest();
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 4);
    pool->shutdownAsync([] {});
}

void SessionPoolTest::aTransientFailureBacksOffUntilTheForegroundReachesTheServer()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    m_fake->setAutoLoginOk(false);
    auto pool = makePool();
    QSignalSpy attention(pool.get(), &SessionPool::hostNeedsAttention);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(handleFor(a), 3000);
    m_fake->setSticky(handleFor(a), QStringLiteral("login_failed"),
                      QJsonObject{{QStringLiteral("message"), QStringLiteral("connection refused")}});
    drain(*pool);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Stopped);
    QVERIFY(!attention.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!pool->holdsStore(a), 3000);   // retired, latch free
    // Within the backoff: no new attempt.
    m_now += 30 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    // The foreground reaches the same server: the backoff resets at once.
    pool->noteForegroundHomeserverReachable(QStringLiteral("https://example.org/"));
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 2, 3000);
    pool->shutdownAsync([] {});
}

void SessionPoolTest::accountsThatMustNotRunNeverStart()
{
    const QString revoked = uidOf(1);
    const QString proven = uidOf(2);
    const QString missing = uidOf(3);
    const QString legacy = uidOf(4);
    // A keyring shared between installs: every account saved by this install
    // has current items of its own, except `legacy`, marked below.
    m_store.setShared(true);
    m_fake->setAutoLoginOk(false);
    for (const QString &uid : {revoked, proven, missing, legacy}) {
        saveAccount(uid);
        QCOMPARE(m_settings->setKeepRunningFor(uid, true), SettingsManager::KeepRunningResult::Ok);
    }
    m_accounts->markSessionRevoked(revoked);
    m_proofs->recordHard(proven, QStringLiteral("DEV-u2"));
    // `legacy`'s items were adopted from before install scoping and never
    // verified (design §9.2, R1-M7).
    {
        QSettings s;
        const QString slug = matrix::app_data::safeUserSlug(legacy);
        s.setValue(QStringLiteral("accounts/") + slug + QStringLiteral("/keyringItems"),
                   QStringLiteral("adopted-unverified"));
        s.setValue(QStringLiteral("accounts/") + slug + QStringLiteral("/keyringDevice"),
                   QStringLiteral("DEV-u4"));
        s.sync();
    }
    auto pool = makePool();
    QSignalSpy attention(pool.get(), &SessionPool::hostNeedsAttention);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    // `missing`: the precheck refuses (no crypto store), nothing is created
    // and it is never retried.
    QTRY_VERIFY_WITH_TIMEOUT(handleFor(missing), 3000);
    m_fake->setSticky(handleFor(missing), QStringLiteral("login_failed"),
                      QJsonObject{{QStringLiteral("stage"), QStringLiteral("store")},
                                  {QStringLiteral("reason"), QStringLiteral("crypto_account_missing")}});
    drain(*pool);
    QCOMPARE(pool->liveState(missing), AccountActivityModel::EncryptionDataMissing);
    for (int i = 0; i < 5; ++i) {
        m_now += 61 * 60 * 1000;
        pool->admitNowForTest();
        QCoreApplication::processEvents();
    }
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    QCOMPARE(pool->liveState(revoked), AccountActivityModel::NeedsSignIn);
    QCOMPARE(pool->liveState(proven), AccountActivityModel::NeedsSignIn);
    QCOMPARE(pool->liveState(legacy), AccountActivityModel::OpenToFinishSetup);
    QCOMPARE(m_store.reads(revoked) + m_store.reads(proven) + m_store.reads(legacy), 0);
    pool->shutdownAsync([] {});
}

// The click path, and R1-C2: the store is free only when Rust holds nothing
// AND the handback landed, so the foreground's first read sees the rotation.
void SessionPoolTest::releaseRetiresHandsBackThenFreesTheStore()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token-at-start"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    QCOMPARE(h->restoredAccess, QStringLiteral("a-token-at-start"));
    // The session rotated after its last drained write.
    m_fake->setFinalTokens(h, QStringLiteral("a-token-final"), QStringLiteral("a-refresh-final"));
    m_store.setWriteLatencyMs(300);
    SessionPool::ReleaseResult result = SessionPool::ReleaseResult::Cancelled;
    QString keyringAtFree;
    bool hostRetiringAtFree = true;
    bool done = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        result = r;
        keyringAtFree = m_store.value(a, QStringLiteral("accessToken"));
        hostRetiringAtFree = HostSignInRetirements::process().contains(a);
        done = true;
    });
    QVERIFY(!done);   // never inside the call
    QCOMPARE(pool->liveState(a), AccountActivityModel::Stopping);
    QVERIFY(pool->holdsStore(a));
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(result, SessionPool::ReleaseResult::Free);
    QCOMPARE(keyringAtFree, QStringLiteral("a-token-final"));
    QVERIFY(!hostRetiringAtFree);
    QCOMPARE(m_fake->log(h).mid(m_fake->log(h).indexOf(QStringLiteral("shutdownTasks"))),
             (QStringList{QStringLiteral("shutdownTasks"), QStringLiteral("finalSessionTokens"),
                          QStringLiteral("tokenLedgerRetain"), QStringLiteral("destroyAndReport"),
                          QStringLiteral("tokenLedgerLatest")}));
    QVERIFY(!pool->holdsStore(a));
    QVERIFY(m_registry->records().isEmpty());
    QCOMPARE(pool->epochFor(a), quint64(1));
    // Released for the foreground: never restarted behind the switch's back.
    m_now += 10000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    pool->shutdownAsync([] {});
}

// R1-C3: a restore can rotate tokens and send the unsent queue; a Starting
// host is retired only once it has a login result.
void SessionPoolTest::aStartingHostIsRetiredOnlyAfterItsLoginResult()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    m_fake->setHoldStarting(true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(handleFor(a), 3000);
    FakeHandle *h = handleFor(a);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Starting);
    bool done = false;
    SessionPool::ReleaseResult result = SessionPool::ReleaseResult::Cancelled;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        result = r;
        done = true;
    });
    for (int i = 0; i < 5; ++i) {
        drain(*pool);
        QCoreApplication::processEvents();
    }
    QCOMPARE(m_fake->log(h).count(QStringLiteral("shutdownTasks")), 0);
    QVERIFY(!done);
    m_fake->setFinalTokens(h, QStringLiteral("rotated-at-restore"), QString());
    m_fake->releaseStarting(h);
    drain(*pool);
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(result, SessionPool::ReleaseResult::Free);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("rotated-at-restore"));
    pool->shutdownAsync([] {});
}

void SessionPoolTest::anUnconfirmedCloseKeepsTheStoreBusy()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    m_fake->setDestroyReport(QStringLiteral("store_closed=false waited_ms=5000 still_open=crypto"));
    m_fake->setAnyHeld(storePathOf(a), 1);
    SessionPool::ReleaseResult first = SessionPool::ReleaseResult::Free;
    const quint64 ticket =
        pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) { first = r; });
    QTRY_COMPARE_WITH_TIMEOUT(pool->liveState(a), AccountActivityModel::StoreBusy, 5000);
    QCOMPARE(m_registry->records().size(), 1);   // still Retiring
    QCOMPARE(m_registry->records().first().state,
             lightning::accounts::StoreOwnershipRegistry::State::Retiring);
    pool->cancelRelease(ticket);
    QCOMPARE(first, SessionPool::ReleaseResult::Cancelled);
    // Asked again: busy at once, and never restarted.
    bool again = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        again = r == SessionPool::ReleaseResult::StoreBusy;
    });
    QTRY_VERIFY_WITH_TIMEOUT(again, 2000);
    pool->setForegroundUserId(kForeground);
    m_now += 3600 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    pool->shutdownAsync([] {});
}

// A switch away from a pinned account leaves the foreground's handle of its
// store retiring for a moment: the host waits (no keyring read, no open) for
// as long as that close may still come, and starts once it is confirmed. A
// close slower than 15 s that succeeds is NOT "Store busy" (review M6).
void SessionPoolTest::aStoreTheForegroundStillHoldsIsWaitedForWithoutKeyringReads()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    using Registry = lightning::accounts::StoreOwnershipRegistry;
    matrix::app_data::AccountIdentity identity;
    QVERIFY(m_settings->resolveSavedIdentity(a, &identity));
    quint64 foregroundSerial = 0;
    QCOMPARE(m_registry->claimOpen(identity.rustStorePath, identity.effectiveStoreSlug(),
                                   Registry::Role::Foreground, &foregroundSerial),
             Registry::ClaimVerdict::Granted);
    m_registry->noteReleasing(foregroundSerial);
    m_store.resetCounts();
    auto pool = makePool();
    QSignalSpy attention(pool.get(), &SessionPool::hostNeedsAttention);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    for (int i = 0; i < 20; ++i) {
        m_now += 1000;   // 20 s: past the old 15 s cut-off
        pool->admitNowForTest();
        QCoreApplication::processEvents();
    }
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    QCOMPARE(m_store.reads(a), 0);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Stopping);
    QVERIFY(attention.isEmpty());
    // Closed at last: the host starts.
    m_registry->noteClosed(foregroundSerial, true);
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 1, 3000);
    pool->shutdownAsync([] {});
}

void SessionPoolTest::removalSignsOutWritesNothingBackAndAwaitsThatStore()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_fake->setFinalTokens(h, QStringLiteral("must-not-be-written"), QString());
    m_store.resetCounts();
    SessionPool::RemovalResult result;
    bool done = false;
    pool->retireForRemoval(a, [&](SessionPool::RemovalResult r) {
        result = r;
        done = true;
    });
    // The sign-out dispatched on the pool; its sticky result is drained.
    QTRY_VERIFY_WITH_TIMEOUT(m_fake->calls(QStringLiteral("logout")) == 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT((drain(*pool), done), 10000);
    QVERIFY(result.storeFree);
    QCOMPARE(result.serverSignOut, SessionPool::ServerSignOut::Ok);
    QCOMPARE(m_fake->log(h).count(QStringLiteral("finalSessionTokens")), 0);
    QCOMPARE(m_store.writes(a), 0);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token"));
    // The pool deletes nothing: the record is AppController's to remove.
    QVERIFY(m_settings->hasSavedAccount(a));
    QCOMPARE(m_store.deletes(a), 0);
    // Never restarted.
    m_now += 3600 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);

    // A non-live account: NotLive, store free.
    bool notLive = false;
    pool->retireForRemoval(uidOf(9), [&](SessionPool::RemovalResult r) {
        notLive = r.storeFree && r.serverSignOut == SessionPool::ServerSignOut::NotLive;
    });
    QTRY_VERIFY_WITH_TIMEOUT(notLive, 2000);
    pool->shutdownAsync([] {});
}

// R1-C1: the foreground's classification, the per-account proof, the revoked
// mark, retire without handback, then the signal; nothing deleted.
void SessionPoolTest::aRevokedHostRecordsTheProofAndIsNeverRestarted()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    QStringList order;
    connect(pool.get(), &SessionPool::hostSignInExpired, this, [&](const QString &uid) {
        order << QStringLiteral("signal");
        QCOMPARE(uid, a);
        QVERIFY(m_proofs->hardFor(a).has_value());
        QVERIFY(m_accounts->sessionRevoked(a));
    });
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_store.resetCounts();
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("dead")},
                                      {QStringLiteral("revoked"), true},
                                      {QStringLiteral("revoked_kind"), QStringLiteral("unknown_token")},
                                      {QStringLiteral("soft_logout"), false},
                                      {QStringLiteral("had_refresh_token"), QJsonValue()}});
    drain(*pool);
    QCOMPARE(order, QStringList{QStringLiteral("signal")});
    QCOMPARE(m_proofs->hardFor(a)->deviceId, QStringLiteral("DEV-u1"));
    QCOMPARE(pool->liveState(a), AccountActivityModel::NeedsSignIn);
    QTRY_VERIFY_WITH_TIMEOUT(!pool->holdsStore(a), 5000);
    QCOMPARE(m_fake->log(h).count(QStringLiteral("finalSessionTokens")), 0);
    QCOMPARE(m_store.writes(a), 0);
    QCOMPARE(m_store.deletes(a), 0);
    QVERIFY(m_settings->hasSavedAccount(a));
    m_now += 3600 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    pool->shutdownAsync([] {});
}

void SessionPoolTest::aFailedTokenWritePausesSyncAndTheSendQueueAndRetries()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    pool->setTokenWriteRetryMsForTest(50);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_store.setFailWrites(true);
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("a-rotated")},
                                      {QStringLiteral("refresh_token"), QStringLiteral("r-rotated")},
                                      {QStringLiteral("generation"), 1}});
    drain(*pool);
    QTRY_VERIFY_WITH_TIMEOUT(!h->sendQueueEnabled, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->log(h).count(QStringLiteral("stopSync")), 1, 3000);
    // Paused, never retired, nothing deleted; the old pair stays at rest.
    QVERIFY(pool->holdsStore(a));
    QCOMPARE(pool->liveState(a), AccountActivityModel::Running);
    QCOMPARE(m_store.deletes(a), 0);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token"));
    // The keyring answers again: the retry writes the session's pair now.
    m_store.setFailWrites(false);
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-rotated"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(h->sendQueueEnabled, 3000);
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(m_fake->log(h).count(QStringLiteral("startSync")) == 2, 3000);
    pool->shutdownAsync([] {});
}

// 24a485c5 for a BACKGROUND account: matrix-sdk holds the request it retries
// with the new access token until the rotation is answered, so the answer
// must come after the keyring write, never before (and a failed write still
// answers, so a broken keyring never holds sync for the wait's bound).
// Mutations: answering in onCustody before the write fails the "nothing yet"
// check; no answer at all fails the QTRY.
void SessionPoolTest::aBackgroundRotationIsAnsweredOnlyOnceTheKeyringHoldsIt()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    pool->setTokenWriteRetryMsForTest(60000);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_store.setWriteLatencyMs(400);
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("a-rotated")},
                                      {QStringLiteral("refresh_token"), QStringLiteral("r-rotated")},
                                      {QStringLiteral("generation"), 1},
                                      {QStringLiteral("ledger_generation"), 7}});
    drain(*pool);
    QCoreApplication::processEvents();
    QVERIFY2(m_fake->persistedAnswers(h).isEmpty(),
             "a rotation was answered before its keyring write finished");
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->persistedAnswers(h).size(), 1, 5000);
    QCOMPARE(m_fake->persistedAnswers(h).constFirst(), qMakePair(quint64(7), true));
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-rotated"));

    // The same pair again (custody re-taken for nothing): the keyring already
    // holds it, so it is answered at once and nothing is written.
    m_store.setWriteLatencyMs(0);
    const int writesBefore = m_store.writes(a);
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("a-rotated")},
                                      {QStringLiteral("refresh_token"), QStringLiteral("r-rotated")},
                                      {QStringLiteral("generation"), 2},
                                      {QStringLiteral("ledger_generation"), 8}});
    drain(*pool);
    QCOMPARE(m_fake->persistedAnswers(h).size(), 2);
    QCOMPARE(m_fake->persistedAnswers(h).at(1), qMakePair(quint64(8), true));
    QCOMPARE(m_store.writes(a), writesBefore);

    // A keyring that refuses: answered NOT saved (the wait ends), and the
    // pause takes over.
    m_store.setFailWrites(true);
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("a-rotated-2")},
                                      {QStringLiteral("refresh_token"), QStringLiteral("r-rotated-2")},
                                      {QStringLiteral("generation"), 3},
                                      {QStringLiteral("ledger_generation"), 9}});
    drain(*pool);
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->persistedAnswers(h).size(), 3, 5000);
    QCOMPARE(m_fake->persistedAnswers(h).at(2), qMakePair(quint64(9), false));
    QTRY_VERIFY_WITH_TIMEOUT(!h->sendQueueEnabled, 3000);
    m_store.setFailWrites(false);
    pool->shutdownAsync([] {});
}

// la1-A N1/N2: a release that answered Free whose switch was then refused on
// the spot (or answered StoreBusy) left the account in the "released for the
// foreground" set for the rest of the process, so its pin never ran again.
// Mutation: without abandonRelease() the second create never happens (the
// release test above asserts exactly that without it).
void SessionPoolTest::anAbandonedReleaseLetsThePinRunAgain()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    SessionPool::ReleaseResult result = SessionPool::ReleaseResult::Cancelled;
    bool done = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        result = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(result, SessionPool::ReleaseResult::Free);
    m_now += 10000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    // The foreground never changed (the switch was refused): hand it back.
    pool->abandonRelease(a);
    // The foreground account itself is never handed to the pool this way.
    pool->abandonRelease(kForeground);
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 2, 5000);
    QCOMPARE(pool->liveState(kForeground), AccountActivityModel::Active);
    pool->shutdownAsync([] {});
}

// §18 round 1, MUST 1: the click path answered Free from the latch alone, and
// the latch frees once the handback SETTLED, written or not. A refused
// handback left the keyring on the older pair (for MAS, a consumed refresh
// token) and the foreground read it. Now the pair is parked for the
// foreground, which writes it by the same compare-and-swap right before its
// read (RustSdkMatrixClient::persistRetiredSessionTokens). Mutation: drop the
// parkUnsaved() call and hasUnsaved() is false.
void SessionPoolTest::aRefusedHandbackIsParkedForTheForegroundsRead()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token-at-start"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_fake->setFinalTokens(h, QStringLiteral("a-token-final"), QStringLiteral("a-refresh-final"));
    m_store.setFailWrites(true);
    SessionPool::ReleaseResult result = SessionPool::ReleaseResult::Cancelled;
    bool done = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        result = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    // The store is free (nothing holds it, nothing is still being written),
    // and the session's last pair is waiting for the foreground's read.
    QCOMPARE(result, SessionPool::ReleaseResult::Free);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token-at-start"));
    QVERIFY(HostSignInRetirements::process().hasUnsaved(a));
    const auto parked = HostSignInRetirements::process().takeUnsaved(a);
    QCOMPARE(parked.size(), 1);
    QCOMPARE(parked.first().tokens.accessToken, QStringLiteral("a-token-final"));
    QCOMPARE(parked.first().tokens.refreshToken, QStringLiteral("a-refresh-final"));
    QCOMPARE(parked.first().deviceId, QStringLiteral("DEV-u1"));
    m_store.setFailWrites(false);
    pool->shutdownAsync([] {});
}

// ... and a handback the keyring has not answered (timed out, still inside
// the store) can still land AFTER the foreground's read: the store is not
// reported free until it returned (the release waits for it within its
// budget, §18 round 3). Mutation: drop the write check in settleRelease()
// and the release answers Free while the write is stranded.
void SessionPoolTest::aTimedOutHandbackKeepsTheStoreUntilTheWriteReturns()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token-at-start"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    pool->keyringForTest()->setTimeoutsForTest(3000, 200);
    FakeHandle *h = handleFor(a);
    m_fake->setFinalTokens(h, QStringLiteral("a-token-final"), QStringLiteral("a-refresh-final"));
    m_store.setWriteLatencyMs(1500);
    SessionPool::ReleaseResult result = SessionPool::ReleaseResult::Cancelled;
    bool done = false;
    bool writeStillInFlightWhenAnswered = true;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        result = r;
        done = true;
        writeStillInFlightWhenAnswered = pool->keyringForTest()->writeInFlightFor(a);
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY2(!writeStillInFlightWhenAnswered, "answered while the write could still land");
    QCOMPARE(result, SessionPool::ReleaseResult::Free);
    QVERIFY(HostSignInRetirements::process().hasUnsaved(a));
    HostSignInRetirements::process().takeUnsaved(a);
    m_store.setWriteLatencyMs(0);
    pool->shutdownAsync([] {});
}

// L2: a runtime off whose budget for Starting hosts ran out released the pool
// while a host was still restoring: its handle leaked and its store stayed
// claimed. Now the pool answers only once that host retired. Mutation:
// restore `|| overBudget` in checkShutdownDrained.
void SessionPoolTest::aRuntimeOffWaitsForAHostStillRestoring()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    m_fake->setHoldStarting(true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(handleFor(a), 3000);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Starting);
    bool done = false;
    pool->shutdownAsync([&done] { done = true; });
    m_now += SessionPool::kShutdownStartingBudgetMs + 1000;
    QTest::qWait(3 * SessionPool::kShutdownPollMs);
    QVERIFY2(!done, "the pool was released under a host still restoring");
    QVERIFY(pool->holdsStore(a));
    // The restore ends: the host retires, and only then is the pool done.
    m_fake->releaseStarting(handleFor(a));
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(m_fake->calls(QStringLiteral("destroyAndReport")), 1);
}

// L1: the account opened in the foreground while its background start was
// still reading the keyring: the read is dropped and no host is created for
// the account now in front. Mutation: remove the m_reading branch in
// setForegroundUserId and the createHost re-check (a second handle opens).
void SessionPoolTest::aForegroundOpenDropsABackgroundKeyringReadInFlight()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    m_store.setReadLatencyMs(400);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(pool->liveState(a), AccountActivityModel::Starting, 3000);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);   // still reading
    pool->setForegroundUserId(a);
    QTest::qWait(800);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Active);
    m_store.setReadLatencyMs(0);
    pool->shutdownAsync([] {});
}

// L7: with Labs on and nothing pinned or hosted, the lag probe, the admission
// tick and the memory read stopped waking the process; a pin starts them.
void SessionPoolTest::nothingPinnedOrHostedStopsTheTimers()
{
    auto pool = makePool();
    startPool(*pool);
    QTRY_VERIFY_WITH_TIMEOUT(!pool->idleTimersRunningForTest(), 2000);
    const QString a = uidOf(1);
    saveAccount(a);
    QCOMPARE(pool->setKeepRunning(a, true), SessionPool::PinResult::Ok);
    QTRY_VERIFY_WITH_TIMEOUT(pool->idleTimersRunningForTest(), 2000);
    QCOMPARE(pool->setKeepRunning(a, false), SessionPool::PinResult::Ok);
    QTRY_VERIFY_WITH_TIMEOUT(!pool->idleTimersRunningForTest(), 5000);
    pool->shutdownAsync([] {});
}

// R2-1 + R2-2: a live write P1 the keyring did not answer in time, the
// session's last pair P2 refused while the keyring is wedged (parked), then P1
// lands. Back in the pool, P2 is written BEFORE the account's sign-in is read,
// over P1, because P1 is the host's own pair. Mutations: park without the
// accepted digests (or drop TimedOut digests from the book) and the
// compare-and-swap refuses, so the keyring keeps P1; drop writeParked() from
// admit() and the new host restores on P1.
void SessionPoolTest::aParkedPairIsWrittenOverTheHostsOwnStrandedLiveWriteBeforeItStartsAgain()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token-at-start"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    pool->setTokenWriteRetryMsForTest(60000);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    pool->keyringForTest()->setTimeoutsForTest(3000, 200);
    FakeHandle *h = handleFor(a);
    m_store.setWriteLatencyMs(900);
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("a-rotated")},
                                      {QStringLiteral("refresh_token"), QStringLiteral("r-rotated")},
                                      {QStringLiteral("generation"), 1},
                                      {QStringLiteral("ledger_generation"), 3}});
    drain(*pool);
    QTRY_VERIFY_WITH_TIMEOUT(pool->keyringForTest()->wedged(), 3000);
    m_fake->setFinalTokens(h, QStringLiteral("a-token-final"), QStringLiteral("a-refresh-final"));
    bool done = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(HostSignInRetirements::process().hasUnsaved(a));
    // P1 lands after all.
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-rotated"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!pool->keyringForTest()->wedged(), 5000);
    m_store.setWriteLatencyMs(0);
    // Back to the pool (as after StoreBusy): P2 first, then the host.
    const int created = m_fake->calls(QStringLiteral("createBackground"));
    pool->abandonRelease(a);
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-token-final"), 5000);
    QVERIFY(!HostSignInRetirements::process().hasUnsaved(a));
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), created + 1,
                              5000);
    QTRY_VERIFY_WITH_TIMEOUT(handleFor(a) && handleFor(a) != h
                                 && !handleFor(a)->restoredAccess.isEmpty(), 5000);
    QCOMPARE(handleFor(a)->restoredAccess, QStringLiteral("a-token-final"));
    pool->shutdownAsync([] {});
}

// R2-3 + L-a: at quit the pool writes the parked pairs through its worker,
// but not one whose account still has a write inside the store (it stays
// parked rather than queue behind a write that may never return), and the
// quit keeps its budget. Mutation: drop the writeInFlightFor() skip and b's
// pair is taken.
void SessionPoolTest::aQuitLeavesAParkedPairBehindAWriteStillInFlightAndStaysInBudget()
{
    const QString a = uidOf(1);
    const QString b = uidOf(2);
    saveAccount(a, QStringLiteral("a-start"));
    saveAccount(b, QStringLiteral("b-start"));
    auto pool = makePool();
    startPool(*pool);
    pool->keyringForTest()->setTimeoutsForTest(3000, 5000);
    const auto parked = [](const QString &uid, const QString &start, const QString &last) {
        return HostSignInRetirements::UnsavedHandback{
            uid, QStringLiteral("DEV-") + uid.mid(1, uid.indexOf(QLatin1Char(':')) - 1),
            custody::accessTokenDigest(start), {}, SessionTokenPair{last, QString()}};
    };
    HostSignInRetirements::process().parkUnsaved(parked(a, QStringLiteral("a-start"),
                                                        QStringLiteral("a-final")));
    HostSignInRetirements::process().parkUnsaved(parked(b, QStringLiteral("b-start"),
                                                        QStringLiteral("b-final")));
    // b's own write, still inside the store for the whole quit.
    m_store.setWriteLatencyMs(2000);
    KeyringWorker::WriteRequest inFlight;
    inFlight.userId = b;
    inFlight.deviceId = parked(b, QString(), QString()).deviceId;
    inFlight.kind = KeyringWorker::WriteKind::Live;
    inFlight.tokens = SessionTokenPair{QStringLiteral("b-live"), QString()};
    pool->keyringForTest()->write(inFlight, [](const KeyringWorker::WriteResult &) {});
    QTRY_VERIFY_WITH_TIMEOUT(pool->keyringForTest()->writeInFlightFor(b), 2000);
    pool->beginQuit();
    QElapsedTimer quit;
    quit.start();
    pool->waitForQuit(600);
    QVERIFY2(quit.elapsed() < 3000, qPrintable(QString::number(quit.elapsed())));
    QVERIFY(HostSignInRetirements::process().hasUnsaved(b));
    QVERIFY(!HostSignInRetirements::process().hasUnsaved(a));   // taken, queued
    // Let b's write return before the pool and the store go away.
    QTRY_VERIFY_WITH_TIMEOUT(!pool->keyringForTest()->writeInFlightFor(b), 5000);
    m_store.setWriteLatencyMs(0);
}

// R3-1: a parked pair being written through the worker took it out of
// HostSignInRetirements while no host held the store, so holdsStore() was
// false and today's switch read the keyring before the write landed. Now the
// account holds its sign-in until the write returns, and a release for it
// answers Free only after that. Mutations: holdsSignIn() without the write
// check (false while writing); settleRelease() without it (Free at once,
// before the keyring holds the pair).
void SessionPoolTest::aSwitchWaitsForAParkedPairStillBeingWritten()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-start"));
    m_settings->setKeepRunningFor(a, true);
    HostSignInRetirements::process().parkUnsaved(HostSignInRetirements::UnsavedHandback{
        a, QStringLiteral("DEV-u1"), custody::accessTokenDigest(QStringLiteral("a-start")), {},
        SessionTokenPair{QStringLiteral("a-final"), QStringLiteral("r-final")}});
    m_store.setWriteLatencyMs(800);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(pool->keyringForTest()->writeInFlightFor(a), 3000);
    QVERIFY(!HostSignInRetirements::process().hasUnsaved(a));   // taken for the write
    QVERIFY(!pool->holdsStore(a));
    QVERIFY(pool->holdsSignIn(a));   // so the switch goes through a release
    QString keyringWhenAnswered;
    SessionPool::ReleaseResult result = SessionPool::ReleaseResult::Cancelled;
    bool done = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        result = r;
        done = true;
        keyringWhenAnswered = m_store.value(a, QStringLiteral("accessToken"));
    });
    QTest::qWait(200);
    QVERIFY2(!done, "the switch was let through while the write was still running");
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(result, SessionPool::ReleaseResult::Free);
    QCOMPARE(keyringWhenAnswered, QStringLiteral("a-final"));
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    m_store.setWriteLatencyMs(0);
    pool->shutdownAsync([] {});
}

// R4-1: a removal of an account with no host, whose parked pair is being
// written, answers only once that write returned (nothing deleted while it
// can still land), and a pair parked for it is discarded, not written later.
// Mutation: finishRemoval() on writeInFlightFor() alone and the answer comes
// in the window where the write answered but its callback has not run; drop
// the takeUnsaved() and the second parked pair survives the removal.
void SessionPoolTest::aRemovalWaitsForAParkedPairsWriteAndDiscardsThePair()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-start"));
    m_settings->setKeepRunningFor(a, true);
    HostSignInRetirements::process().parkUnsaved(HostSignInRetirements::UnsavedHandback{
        a, QStringLiteral("DEV-u1"), custody::accessTokenDigest(QStringLiteral("a-start")), {},
        SessionTokenPair{QStringLiteral("a-final"), QString()}});
    m_store.setWriteLatencyMs(800);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(pool->keyringForTest()->writeInFlightFor(a), 3000);
    // In flight counts a write still QUEUED, which the removal simply
    // cancels (nothing can land). The case is the write already inside the
    // store: let the worker start it (800 ms in the store from then on).
    QTest::qWait(200);
    QVERIFY(pool->keyringForTest()->writeInFlightFor(a));
    // Another pair parked meanwhile (a retirement of an earlier host).
    HostSignInRetirements::process().parkUnsaved(HostSignInRetirements::UnsavedHandback{
        a, QStringLiteral("DEV-u1"), custody::accessTokenDigest(QStringLiteral("a-start")), {},
        SessionTokenPair{QStringLiteral("a-later"), QString()}});
    bool answered = false;
    bool writeLandedFirst = false;
    pool->retireForRemoval(a, [&](SessionPool::RemovalResult) {
        answered = true;
        writeLandedFirst = !pool->holdsSignIn(a);
    });
    QVERIFY(!HostSignInRetirements::process().hasUnsaved(a));
    QTest::qWait(200);
    QVERIFY2(!answered, "the removal answered while the write could still land");
    QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
    QVERIFY(writeLandedFirst);
    QVERIFY(!HostSignInRetirements::process().hasUnsaved(a));
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    m_store.setWriteLatencyMs(0);
    pool->shutdownAsync([] {});
}

// ---- review round 1 ------------------------------------------------------------------------

// H1: a lost connection sends stop_sync to the pool; an unpin a moment later
// retires the host. The destroy frees the client stop_sync still holds, so it
// must run after stop_sync returned, never beside it.
void SessionPoolTest::aRetireAfterALostConnectionWaitsForTheStop()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_fake->setStopSyncDelayMs(800);
    m_fake->setSticky(h, QStringLiteral("room_list_sync_state"),
                      QJsonObject{{QStringLiteral("state"), QStringLiteral("offline")}});
    drain(*pool);
    QTRY_VERIFY_WITH_TIMEOUT(m_fake->log(h).contains(QStringLiteral("stopSync")), 2000);
    QCOMPARE(pool->setKeepRunning(a, false), SessionPool::PinResult::Ok);
    QTRY_VERIFY_WITH_TIMEOUT(!pool->holdsStore(a), 10000);
    QCOMPARE(m_fake->destroyOverlaps(), 0);
    QCOMPARE(m_fake->callsAfterDestroy(), 0);
    const QStringList log = m_fake->log(h);
    QVERIFY(log.indexOf(QStringLiteral("stopSync")) < log.indexOf(QStringLiteral("shutdownTasks")));
    // The quit path (every host retired at once) meets the same queue.
    pool->shutdownAsync([] {});
}

// M1 and M2: an unpin retires the host and queues its handback; a removal a
// moment later cancels that write. The cancelled handback still finishes the
// retirement (the record closes, the latch frees), the removal reports the
// store free, and the final pair never reaches the keyring.
void SessionPoolTest::unpinThenRemoveFinishesTheHandbackAndWritesNothing()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token-at-start"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_fake->setFinalTokens(h, QStringLiteral("a-final-must-not-land"),
                           QStringLiteral("a-final-refresh"));
    m_store.setWriteLatencyMs(400);
    QCOMPARE(pool->setKeepRunning(a, false), SessionPool::PinResult::Ok);
    // The retirement ran; its handback is queued or inside the store.
    QTRY_VERIFY_WITH_TIMEOUT(pool->keyringForTest()->writeInFlightFor(a), 5000);
    QVERIFY(HostSignInRetirements::process().contains(a));
    SessionPool::RemovalResult result;
    bool done = false;
    pool->retireForRemoval(a, [&](SessionPool::RemovalResult r) {
        result = r;
        done = true;
    });
    // Well inside the 15 s budget: nothing waits for a callback that never runs.
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(result.storeFree);
    QCOMPARE(result.serverSignOut, SessionPool::ServerSignOut::NotLive);
    QVERIFY(!pool->keyringForTest()->writeInFlightFor(a));
    QVERIFY(!HostSignInRetirements::process().contains(a));
    QVERIFY(!pool->latchesForTest()->isArmed(a));
    QVERIFY(m_registry->records().isEmpty());
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token-at-start"));
    // Nothing more lands later either.
    m_store.setWriteLatencyMs(0);
    QTest::qWait(500);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token-at-start"));
    pool->shutdownAsync([] {});
}

// M4: the keyring probe of an OAuth revocation answers after the click path
// retired the host and the account became the foreground's: the answer is
// about a session that is gone, so no proof, no revoked mark, no signal, and
// the foreground's Active state stays.
void SessionPoolTest::aLateRevocationProbeForARetiredHostChangesNothing()
{
    const QString a = uidOf(1);
    m_settings->saveSession(QStringLiteral("https://example.org"), a, QStringLiteral("DEV-u1"),
                            QStringLiteral("token-of-oauth"), QStringLiteral("refresh-of-oauth"),
                            QStringLiteral("oauth"), QStringLiteral("client-fixture"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    QSignalSpy expired(pool.get(), &SessionPool::hostSignInExpired);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    QVERIFY(h->oauth);
    // The keyring holds no refresh token either, so the probe, when it
    // answers, proves the session ended (Hard): what a stale answer would act on.
    m_store.setValue(a, QStringLiteral("refreshToken"), QString());
    m_store.setReadLatencyMs(1000);   // the probe answers late (two reads, inside 3 s)
    // OAuth without a refresh token: the classification asks the keyring.
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("dead")},
                                      {QStringLiteral("revoked"), true},
                                      {QStringLiteral("revoked_kind"), QStringLiteral("unknown_token")},
                                      {QStringLiteral("soft_logout"), QJsonValue()},
                                      {QStringLiteral("had_refresh_token"), false}});
    drain(*pool);
    QVERIFY(expired.isEmpty());   // waiting on the probe
    bool released = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult) { released = true; });
    QTRY_VERIFY_WITH_TIMEOUT(released, 10000);
    pool->setForegroundUserId(a);
    QTest::qWait(3000);   // the probe has answered by now
    QVERIFY(expired.isEmpty());
    QVERIFY(!m_proofs->hardFor(a).has_value());
    QVERIFY(!m_accounts->sessionRevoked(a));
    QCOMPARE(pool->liveState(a), AccountActivityModel::Active);
    pool->shutdownAsync([] {});
}

// M5: a refusal recorded while the account ran in the background (here: the
// keyring held no sign-in) is the foreground's to fix. Once the account has
// been the foreground's, it is asked again when it is a background one.
void SessionPoolTest::openingAnAccountInTheForegroundClearsItsRefusals()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    m_store.setValue(a, QStringLiteral("accessToken"), QString());
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(pool->liveState(a), AccountActivityModel::NeedsSignIn, 3000);
    m_now += 3600 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    // Signed in again in the foreground, then switched away.
    pool->setForegroundUserId(a);
    m_store.setValue(a, QStringLiteral("accessToken"), QStringLiteral("a-token-after-sign-in"));
    pool->setForegroundUserId(kForeground);
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 1, 3000);
    pool->shutdownAsync([] {});
}

// M6: only a close REPORTED unconfirmed is Store busy, and then for good.
void SessionPoolTest::anUnconfirmedForegroundCloseIsStoreBusy()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    using Registry = lightning::accounts::StoreOwnershipRegistry;
    matrix::app_data::AccountIdentity identity;
    QVERIFY(m_settings->resolveSavedIdentity(a, &identity));
    quint64 foregroundSerial = 0;
    QCOMPARE(m_registry->claimOpen(identity.rustStorePath, identity.effectiveStoreSlug(),
                                   Registry::Role::Foreground, &foregroundSerial),
             Registry::ClaimVerdict::Granted);
    m_registry->noteReleasing(foregroundSerial);
    m_registry->noteClosed(foregroundSerial, false);
    QCOMPARE(m_registry->holdOn(identity.rustStorePath, identity.effectiveStoreSlug()),
             Registry::Hold::Unconfirmed);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QCOMPARE(pool->liveState(a), AccountActivityModel::StoreBusy);
    m_now += 3600 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    QCOMPARE(m_store.reads(a), 0);
    pool->shutdownAsync([] {});
}

// M7: an account whose RECORDED store slug differs from its canonical slug
// (delegation, a case variant). The foreground claims under the recorded one;
// so must the host, or the foreground's own retiring handle reads as another
// account's record and the account is Store busy for good.
void SessionPoolTest::aDelegatedStoreSlugWaitsForTheForegroundInsteadOfStoreBusy()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setStoreSlugFor(a, QStringLiteral("delegated-store-u1"));
    m_settings->setKeepRunningFor(a, true);
    matrix::app_data::AccountIdentity identity;
    QVERIFY(m_settings->resolveSavedIdentity(a, &identity));
    QCOMPARE(identity.effectiveStoreSlug(), QStringLiteral("delegated-store-u1"));
    QVERIFY(identity.slug != identity.effectiveStoreSlug());
    using Registry = lightning::accounts::StoreOwnershipRegistry;
    // Labs is on (RustSdkMatrixClient::setLiveAccountsLabs enforces the
    // process registry); unenforced, every claim is Granted and the slug
    // path under test never runs (integration).
    m_registry->setEnforced(true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();   // the keyring read is queued
    // The foreground opens the store meanwhile (as its claim names it).
    quint64 foregroundSerial = 0;
    QCOMPARE(m_registry->claimOpen(identity.rustStorePath, identity.effectiveStoreSlug(),
                                   Registry::Role::Foreground, &foregroundSerial),
             Registry::ClaimVerdict::Granted);
    QTRY_COMPARE_WITH_TIMEOUT(pool->liveState(a), AccountActivityModel::Stopping, 3000);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    // It closes: the host starts.
    m_registry->noteReleasing(foregroundSerial);
    m_registry->noteClosed(foregroundSerial, true);
    m_now += 1100;
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 1, 3000);
    pool->shutdownAsync([] {});
}

// L12: "no_session" signed nothing out on the server.
void SessionPoolTest::aSignOutWithNoSessionIsNotReportedAsDone()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    m_fake->setLogoutResult(QStringLiteral("no_session"));
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    SessionPool::RemovalResult result;
    bool done = false;
    pool->retireForRemoval(a, [&](SessionPool::RemovalResult r) {
        result = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT((drain(*pool), done), 10000);
    QCOMPARE(result.serverSignOut, SessionPool::ServerSignOut::Failed);
    pool->shutdownAsync([] {});
}

// ---- review round 2 ------------------------------------------------------------------------

// The retirement pool was sized from the LIVE hosts alone, and retireHost()
// removes the host before it submits the job: at quit the cap fell 4, 3, 2, 1
// while four retirements were submitted, so QThreadPool queued the last two
// behind one thread. Every retiring host keeps its thread now.
void SessionPoolTest::quitRetiresEveryHostInParallel()
{
    QStringList uids;
    for (int i = 1; i <= SettingsManager::kMaxPinnedAccounts; ++i) {
        const QString uid = uidOf(i, QStringLiteral("q%1.example").arg(i));
        saveAccount(uid);
        QCOMPARE(m_settings->setKeepRunningFor(uid, true), SettingsManager::KeepRunningResult::Ok);
        uids << uid;
    }
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    for (const QString &uid : uids) {
        pool->admitNowForTest();
        bringToRunning(*pool, uid);
    }
    QCOMPARE(pool->liveHostCount(), int(uids.size()));
    QCOMPARE(m_fake->maxConcurrentBlocking(), 0);
    m_fake->setRetireDelayMs(500);   // shutdown and destroy each
    pool->beginQuit();
    QCOMPARE(pool->liveHostCount(), 0);
    // Old code: 1 (live hosts 0, + 1).
    QCOMPARE(pool->retirementForTest()->maxThreadCount(), int(uids.size()) + 1);
    QVERIFY(pool->waitForQuit(20000));
    // All four closes overlapped; the old cap ran at most two at a time.
    QCOMPARE(m_fake->maxConcurrentBlocking(), int(uids.size()));
    for (const QString &uid : uids)
        QVERIFY(m_fake->log(handleFor(uid)).contains(QStringLiteral("destroyAndReport")));
    QCOMPARE(m_fake->destroyOverlaps(), 0);
}

// A removal's "never restarted" and "no keyring writes" were for the life of
// the process: the same account removed and added again stayed NotLive with no
// message, and a live write a stale forbid cancelled was dropped. A fresh
// sign-in (the account opened in the foreground after the removal answered)
// lifts both.
void SessionPoolTest::aRemovedAccountSignedInAgainMayRunInTheBackgroundAgain()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    bool done = false;
    SessionPool::RemovalResult result;
    pool->retireForRemoval(a, [&](SessionPool::RemovalResult r) {
        result = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT((drain(*pool), done), 10000);
    QVERIFY(result.storeFree);
    // Signed in again: AppController removed the record, the user added it.
    saveAccount(a, QStringLiteral("a-token-new"));
    pool->setForegroundUserId(a);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Active);
    pool->setForegroundUserId(kForeground);
    QCOMPARE(pool->setKeepRunning(a, true), SessionPool::PinResult::Ok);
    m_now += 60 * 1000;
    pool->admitNowForTest();
    // Old code: still 1, for the rest of the process.
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 2, 3000);
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    QCOMPARE(h->restoredAccess, QStringLiteral("a-token-new"));
    // Its rotations reach the keyring again.
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("a-rotated")},
                                      {QStringLiteral("refresh_token"), QStringLiteral("r-rotated")},
                                      {QStringLiteral("generation"), 1}});
    drain(*pool);
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-rotated"), 5000);
    QVERIFY(h->sendQueueEnabled);
    pool->shutdownAsync([] {});
}

// A live host's write answered Cancelled by anything but its own removal (or
// a retirement, whose epoch drops the answer) is a write that did not land:
// sync and the send queue pause as for a failed write. It was dropped.
void SessionPoolTest::aLiveWriteCancelledOutsideARemovalPausesInsteadOfVanishing()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token"));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    pool->setTokenWriteRetryMsForTest(50);
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    m_store.setWriteLatencyMs(400);
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("a-rotated")},
                                      {QStringLiteral("refresh_token"), QStringLiteral("r-rotated")},
                                      {QStringLiteral("generation"), 1}});
    drain(*pool);
    QTRY_VERIFY_WITH_TIMEOUT(pool->keyringForTest()->writeInFlightFor(a), 3000);
    pool->keyringForTest()->cancelFor(a);
    // Old code: the Cancelled answer was dropped and nothing paused.
    QTRY_VERIFY_WITH_TIMEOUT(!h->sendQueueEnabled, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->log(h).count(QStringLiteral("stopSync")), 1, 3000);
    QVERIFY(pool->holdsStore(a));
    // The retry writes the session's pair and resumes.
    m_store.setWriteLatencyMs(0);
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-rotated"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(h->sendQueueEnabled, 3000);
    pool->shutdownAsync([] {});
}

QTEST_GUILESS_MAIN(SessionPoolTest)

// la1-M contract (review round 2, N1): an account removed while NOT live
// (AppController's non-live path tombstones the activity row; the pool's
// m_removing never saw it), then signed in again and pinned, is reopened at
// the pool's epoch when its host is admitted, so its row and badge show.
// Fails with: the reopen in SessionPool::beginStart removed (the row stays
// tombstoned and never reads Running).
void SessionPoolTest::anAccountRemovedWhileNotLiveShowsItsRowWhenAdmitted()
{
    const QString a = uidOf(1);
    saveAccount(a, QStringLiteral("a-token"));
    m_activity->removeAccount(a);
    QVERIFY(m_activity->isRemoved(a));
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    QVERIFY(!m_activity->isRemoved(a));
    bool running = false;
    for (int row = 0; row < m_activity->rowCount(); ++row) {
        const QModelIndex index = m_activity->index(row, 0);
        if (m_activity->data(index, AccountActivityModel::UserIdRole).toString() == a) {
            running = m_activity->data(index, AccountActivityModel::LiveStateRole).toInt()
                == int(AccountActivityModel::Running);
        }
    }
    QVERIFY2(running, "the re-added account's activity row never came back");
    pool->shutdownAsync([] {});
}

#include "SessionPoolTest.moc"
