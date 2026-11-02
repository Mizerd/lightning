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

}

class SessionPoolTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void backoffIsOneFiveFifteenSixtyMinutesPlusMinusTwentyPercent();
    void theLagProbeGatesAndDetectsAResume();
    void catchUpCompletesOnRunningAfterSyncWithALowRateOrAfterSixtySeconds();
    void fdHeadroomComesFromConfiguredMaxima();
    void homeserverRulesPauseAndReset();
    void nothingRunsBeforeStartAndStartEnablesLabsOnce();
    void twoHundredAccountsThirtyPinsFourRunAndNothingElseIsRead();
    void nothingStartsBeforeTheGate();
    void oneCatchUpAtATime();
    void aResumeRestartsHostsOneAtATimeAfterTheForeground();
    void aTransientFailureBacksOffUntilTheForegroundReachesTheServer();
    void accountsThatMustNotRunNeverStart();
    void releaseRetiresHandsBackThenFreesTheStore();
    void aRunningHostIsHandedOverForAdoptionWithoutRetiringIt();
    void anAccountThatCannotBeHandedOverStaysWithItsHost();
    void aStartingHostIsRetiredOnlyAfterItsLoginResult();
    void anUnconfirmedCloseKeepsTheStoreBusy();
    void anUnconfirmedCloseThatLooksIdleIsNotHandedToTheForeground();
    void aStoreTheForegroundStillHoldsIsWaitedForWithoutKeyringReads();
    void removalSignsOutWritesNothingBackAndAwaitsThatStore();
    void aRevokedHostRecordsTheProofAndIsNeverRestarted();
    void aFailedTokenWritePausesSyncAndTheSendQueueAndRetries();
    void aRetireAfterALostConnectionWaitsForTheStop();
    void unpinThenRemoveFinishesTheHandbackAndWritesNothing();
    void aLateRevocationProbeForARetiredHostChangesNothing();
    void openingAnAccountInTheForegroundClearsItsRefusals();
    void anUnconfirmedForegroundCloseIsStoreBusy();
    void aDelegatedStoreSlugWaitsForTheForegroundInsteadOfStoreBusy();
    void aSignOutWithNoSessionIsNotReportedAsDone();
    void quitRetiresEveryHostInParallel();
    void aRemovedAccountSignedInAgainMayRunInTheBackgroundAgain();
    void aLiveWriteCancelledOutsideARemovalPausesInsteadOfVanishing();
    void anAccountRemovedWhileNotLiveShowsItsRowWhenAdmitted();

    void aBackgroundRotationIsAnsweredOnlyOnceTheKeyringHoldsIt();
    void anAbandonedReleaseLetsThePinRunAgain();
    void aRefusedHandbackIsParkedForTheForegroundsRead();
    void aTimedOutHandbackKeepsTheStoreUntilTheWriteReturns();
    void aRuntimeOffWaitsForAHostStillRestoring();
    void aForegroundOpenDropsABackgroundKeyringReadInFlight();
    void nothingPinnedOrHostedStopsTheTimers();
    void aParkedPairIsWrittenOverTheHostsOwnStrandedLiveWriteBeforeItStartsAgain();
    void aQuitLeavesAParkedPairBehindAWriteStillInFlightAndStaysInBudget();
    void aSwitchWaitsForAParkedPairStillBeingWritten();
    void aRemovalWaitsForAParkedPairsWriteAndDiscardsThePair();

private:
    void saveAccount(const QString &uid, const QString &token = QString());
    std::unique_ptr<SessionPool> makePool();
    FakeHandle *handleFor(const QString &uid) const;
    QString storePathOf(const QString &uid) const;
    void bringToRunning(SessionPool &pool, const QString &uid);
    void drain(SessionPool &pool) { pool.drainerForTest()->tick(); }
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
    drain(pool);
    QCOMPARE(pool.liveState(uid), AccountActivityModel::Running);
    m_fake->setSticky(h, QStringLiteral("room_list_sync_state"),
                      QJsonObject{{QStringLiteral("state"), QStringLiteral("running")}});
    drain(pool);
    m_now += CatchUpTracker::kRateWindowMs + 100;
    drain(pool);
    m_now += 1100;
    QVERIFY(pool.schedulerForTest()->catchUpHolder().isEmpty());
}

void SessionPoolTest::backoffIsOneFiveFifteenSixtyMinutesPlusMinusTwentyPercent()
{
    const qint64 minute = 60 * 1000;
    QCOMPARE(SessionScheduler::backoffDelayMs(1, 0.5), 1 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(2, 0.5), 5 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(3, 0.5), 15 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(4, 0.5), 60 * minute);
    QCOMPARE(SessionScheduler::backoffDelayMs(9, 0.5), 60 * minute);
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
    QVERIFY(!scheduler.gateOpen());
    scheduler.noteStarted();
    scheduler.startProbe();
    scheduler.stopProbe();
    QVERIFY(!scheduler.gateOpen());
    scheduler.noteForegroundInitialSyncDone();
    QVERIFY(scheduler.gateOpen());
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
    asleep += 11000;
    now += 100;
    scheduler.probeTick();
    QCOMPARE(resumed.size(), 1);
    QVERIFY(scheduler.lagClear());
    QVERIFY(!scheduler.catchUpFree());
    scheduler.noteForegroundInitialSyncDone();
    QVERIFY(scheduler.catchUpFree());

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
    QVERIFY(!tracker.complete(3100, true, true, 5));
    QVERIFY(!tracker.complete(3100, false, true, 6));
    QVERIFY(tracker.complete(3100, true, true, 6));
    CatchUpTracker busy;
    busy.begin(0, 1);
    busy.sample(0, 0);
    busy.sample(3000, 300);
    QVERIFY(!busy.complete(3000, true, true, 2));
    QVERIFY(busy.complete(CatchUpTracker::kCapMs, false, false, 1));
}

void SessionPoolTest::fdHeadroomComesFromConfiguredMaxima()
{
    SessionScheduler scheduler([] { return qint64(0); }, [] { return 0.5; });
    scheduler.setFdSoftLimitForTest(1024);
    QVERIFY(scheduler.fdHeadroomFor(7));
    QVERIFY(!scheduler.fdHeadroomFor(8));
    scheduler.setFdSoftLimitForTest(0);
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
    scheduler.noteStartFinished(hs, QStringLiteral("@b:example.org"));
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
    QVERIFY(scheduler.permanentlyRefused(QStringLiteral("@p:example.org")));
}

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
    const QStringList admissionOrder = m_settings->keepRunningUserIds();
    QCOMPARE(QSet<QString>(admissionOrder.cbegin(), admissionOrder.cend()),
             QSet<QString>(pinned.cbegin(), pinned.cend()));
    for (const QString &uid : admissionOrder) {
        pool->admitNowForTest();
        bringToRunning(*pool, uid);
    }
    QCOMPARE(pool->liveHostCount(), SettingsManager::kMaxPinnedAccounts);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), SettingsManager::kMaxPinnedAccounts);
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

    pool->schedulerForTest()->probeTick();
    m_now += 11000;
    pool->schedulerForTest()->probeTick();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("stopSync")), 0);
    m_asleep += 11000;
    m_now += 100;
    pool->schedulerForTest()->probeTick();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("stopSync")), 2, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(pool->retirementForTest()->activeJobs(), 0, 3000);
    m_now += SessionScheduler::kLagWindowMs + 100;
    pool->admitNowForTest();
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 2);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 3);
    pool->admitNowForTest();
    QCOMPARE(m_fake->calls(QStringLiteral("startSync")), 3);
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
    QTRY_VERIFY_WITH_TIMEOUT(!pool->holdsStore(a), 3000);
    m_now += 30 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
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
    m_store.setShared(true);
    m_fake->setAutoLoginOk(false);
    for (const QString &uid : {revoked, proven, missing, legacy}) {
        saveAccount(uid);
        QCOMPARE(m_settings->setKeepRunningFor(uid, true), SettingsManager::KeepRunningResult::Ok);
    }
    m_accounts->markSessionRevoked(revoked);
    m_proofs->recordHard(proven, QStringLiteral("DEV-u2"));
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
    QVERIFY(!done);
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
    m_now += 10000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    pool->shutdownAsync([] {});
}

void SessionPoolTest::aRunningHostIsHandedOverForAdoptionWithoutRetiringIt()
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
    QCOMPARE(m_registry->records().size(), 1);
    const quint64 serial = m_registry->records().first().serial;
    const quint64 epochBefore = pool->epochFor(a);
    QVERIFY(pool->canAdopt(a));

    const auto ticket = pool->takeForAdoption(a);
    QVERIFY(ticket.has_value());
    QVERIFY(ticket->valid());
    QCOMPARE(ticket->handle, static_cast<void *>(h));
    QCOMPARE(ticket->userId, a);
    QCOMPARE(ticket->storePath, storePathOf(a));
    QCOMPARE(ticket->storeSerial, serial);
    QCoreApplication::processEvents();
    QVERIFY2(!h->log.contains(QStringLiteral("shutdownTasks"))
                 && !h->log.contains(QStringLiteral("destroyAndReport"))
                 && !h->log.contains(QStringLiteral("finalSessionTokens")),
             qPrintable(h->log.join(QLatin1Char(','))));
    QCOMPARE(m_registry->records().size(), 1);
    QCOMPARE(m_registry->records().first().serial, serial);
    QCOMPARE(m_registry->records().first().role,
             lightning::accounts::StoreOwnershipRegistry::Role::Foreground);
    QCOMPARE(m_registry->records().first().state,
             lightning::accounts::StoreOwnershipRegistry::State::Live);
    QVERIFY(!pool->holdsStore(a));
    QCOMPARE(pool->liveHostCount(), 0);
    QVERIFY(pool->epochFor(a) > epochBefore);
    const int pollsBefore = m_fake->calls(QStringLiteral("bgPoll"));
    drain(*pool);
    QCOMPARE(m_fake->calls(QStringLiteral("bgPoll")), pollsBefore);
    m_now += 10000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);
    QVERIFY(!pool->takeForAdoption(a).has_value());
    pool->shutdownAsync([] {});
}

void SessionPoolTest::anAccountThatCannotBeHandedOverStaysWithItsHost()
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
    QVERIFY(!pool->canAdopt(a));
    QVERIFY(!pool->takeForAdoption(a).has_value());
    QVERIFY(pool->holdsStore(a));
    QCOMPARE(pool->liveHostCount(), 1);
    QVERIFY(!pool->takeForAdoption(uidOf(2)).has_value());
    pool->shutdownAsync([] {});
}

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

void SessionPoolTest::anUnconfirmedCloseThatLooksIdleIsNotHandedToTheForeground()
{
    const QString a = uidOf(1);
    saveAccount(a);
    m_settings->setKeepRunningFor(a, true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    bringToRunning(*pool, a);
    m_fake->setDestroyReport(QStringLiteral(
        "store_closed=false waited_ms=0 builds=1 client_alive=false still_open=none "
        "runtime_shutdown=timed_out"));
    m_fake->setAnyHeld(storePathOf(a), 0);
    bool done = false;
    SessionPool::ReleaseResult result = SessionPool::ReleaseResult::Free;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult r) {
        result = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(result, SessionPool::ReleaseResult::StoreBusy);
    QCOMPARE(pool->liveState(a), AccountActivityModel::StoreBusy);
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
    QCOMPARE(m_registry->records().size(), 1);
    QCOMPARE(m_registry->records().first().state,
             lightning::accounts::StoreOwnershipRegistry::State::Retiring);
    pool->cancelRelease(ticket);
    QCOMPARE(first, SessionPool::ReleaseResult::Cancelled);
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
        m_now += 1000;
        pool->admitNowForTest();
        QCoreApplication::processEvents();
    }
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    QCOMPARE(m_store.reads(a), 0);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Stopping);
    QVERIFY(attention.isEmpty());
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
    QTRY_VERIFY_WITH_TIMEOUT(m_fake->calls(QStringLiteral("logout")) == 1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT((drain(*pool), done), 10000);
    QVERIFY(result.storeFree);
    QCOMPARE(result.serverSignOut, SessionPool::ServerSignOut::Ok);
    QCOMPARE(m_fake->log(h).count(QStringLiteral("finalSessionTokens")), 0);
    QCOMPARE(m_store.writes(a), 0);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token"));
    QVERIFY(m_settings->hasSavedAccount(a));
    QCOMPARE(m_store.deletes(a), 0);
    m_now += 3600 * 1000;
    pool->admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 1);

    bool notLive = false;
    pool->retireForRemoval(uidOf(9), [&](SessionPool::RemovalResult r) {
        notLive = r.storeFree && r.serverSignOut == SessionPool::ServerSignOut::NotLive;
    });
    QTRY_VERIFY_WITH_TIMEOUT(notLive, 2000);
    pool->shutdownAsync([] {});
}

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
    QVERIFY(pool->holdsStore(a));
    QCOMPARE(pool->liveState(a), AccountActivityModel::Running);
    QCOMPARE(m_store.deletes(a), 0);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token"));
    m_store.setFailWrites(false);
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-rotated"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(h->sendQueueEnabled, 3000);
    pool->admitNowForTest();
    QTRY_VERIFY_WITH_TIMEOUT(m_fake->log(h).count(QStringLiteral("startSync")) == 2, 3000);
    pool->shutdownAsync([] {});
}

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
    pool->abandonRelease(a);
    pool->abandonRelease(kForeground);
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 2, 5000);
    QCOMPARE(pool->liveState(kForeground), AccountActivityModel::Active);
    pool->shutdownAsync([] {});
}

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
    m_fake->releaseStarting(handleFor(a));
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(m_fake->calls(QStringLiteral("destroyAndReport")), 1);
}

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
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    pool->setForegroundUserId(a);
    QTest::qWait(800);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Active);
    m_store.setReadLatencyMs(0);
    pool->shutdownAsync([] {});
}

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
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-rotated"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!pool->keyringForTest()->wedged(), 5000);
    m_store.setWriteLatencyMs(0);
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
    QVERIFY(!HostSignInRetirements::process().hasUnsaved(a));
    QTRY_VERIFY_WITH_TIMEOUT(!pool->keyringForTest()->writeInFlightFor(b), 5000);
    m_store.setWriteLatencyMs(0);
}

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
    QVERIFY(!HostSignInRetirements::process().hasUnsaved(a));
    QVERIFY(!pool->holdsStore(a));
    QVERIFY(pool->holdsSignIn(a));
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
    QTest::qWait(200);
    QVERIFY(pool->keyringForTest()->writeInFlightFor(a));
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
    pool->shutdownAsync([] {});
}

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
    QTRY_VERIFY_WITH_TIMEOUT(pool->keyringForTest()->writeInFlightFor(a), 5000);
    QVERIFY(HostSignInRetirements::process().contains(a));
    SessionPool::RemovalResult result;
    bool done = false;
    pool->retireForRemoval(a, [&](SessionPool::RemovalResult r) {
        result = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(result.storeFree);
    QCOMPARE(result.serverSignOut, SessionPool::ServerSignOut::NotLive);
    QVERIFY(!pool->keyringForTest()->writeInFlightFor(a));
    QVERIFY(!HostSignInRetirements::process().contains(a));
    QVERIFY(!pool->latchesForTest()->isArmed(a));
    QVERIFY(m_registry->records().isEmpty());
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token-at-start"));
    m_store.setWriteLatencyMs(0);
    QTest::qWait(500);
    QCOMPARE(m_store.value(a, QStringLiteral("accessToken")), QStringLiteral("a-token-at-start"));
    pool->shutdownAsync([] {});
}

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
    m_store.setValue(a, QStringLiteral("refreshToken"), QString());
    m_store.setReadLatencyMs(1000);
    m_fake->setCustody(h, QJsonObject{{QStringLiteral("v"), 1},
                                      {QStringLiteral("has_session"), true},
                                      {QStringLiteral("access_token"), QStringLiteral("dead")},
                                      {QStringLiteral("revoked"), true},
                                      {QStringLiteral("revoked_kind"), QStringLiteral("unknown_token")},
                                      {QStringLiteral("soft_logout"), QJsonValue()},
                                      {QStringLiteral("had_refresh_token"), false}});
    drain(*pool);
    QVERIFY(expired.isEmpty());
    bool released = false;
    pool->releaseForForeground(a, [&](SessionPool::ReleaseResult) { released = true; });
    QTRY_VERIFY_WITH_TIMEOUT(released, 10000);
    pool->setForegroundUserId(a);
    QTest::qWait(3000);
    QVERIFY(expired.isEmpty());
    QVERIFY(!m_proofs->hardFor(a).has_value());
    QVERIFY(!m_accounts->sessionRevoked(a));
    QCOMPARE(pool->liveState(a), AccountActivityModel::Active);
    pool->shutdownAsync([] {});
}

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
    pool->setForegroundUserId(a);
    m_store.setValue(a, QStringLiteral("accessToken"), QStringLiteral("a-token-after-sign-in"));
    pool->setForegroundUserId(kForeground);
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 1, 3000);
    pool->shutdownAsync([] {});
}

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
    m_registry->setEnforced(true);
    auto pool = makePool();
    startPool(*pool);
    pool->noteForegroundInitialSyncDone();
    pool->admitNowForTest();
    quint64 foregroundSerial = 0;
    QCOMPARE(m_registry->claimOpen(identity.rustStorePath, identity.effectiveStoreSlug(),
                                   Registry::Role::Foreground, &foregroundSerial),
             Registry::ClaimVerdict::Granted);
    QTRY_COMPARE_WITH_TIMEOUT(pool->liveState(a), AccountActivityModel::Stopping, 3000);
    QCOMPARE(m_fake->calls(QStringLiteral("createBackground")), 0);
    m_registry->noteReleasing(foregroundSerial);
    m_registry->noteClosed(foregroundSerial, true);
    m_now += 1100;
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 1, 3000);
    pool->shutdownAsync([] {});
}

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
    m_fake->setRetireDelayMs(500);
    pool->beginQuit();
    QCOMPARE(pool->liveHostCount(), 0);
    QCOMPARE(pool->retirementForTest()->maxThreadCount(), int(uids.size()) + 1);
    QVERIFY(pool->waitForQuit(20000));
    QCOMPARE(m_fake->maxConcurrentBlocking(), int(uids.size()));
    for (const QString &uid : uids)
        QVERIFY(m_fake->log(handleFor(uid)).contains(QStringLiteral("destroyAndReport")));
    QCOMPARE(m_fake->destroyOverlaps(), 0);
}

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
    saveAccount(a, QStringLiteral("a-token-new"));
    pool->setForegroundUserId(a);
    QCOMPARE(pool->liveState(a), AccountActivityModel::Active);
    pool->setForegroundUserId(kForeground);
    QCOMPARE(pool->setKeepRunning(a, true), SessionPool::PinResult::Ok);
    m_now += 60 * 1000;
    pool->admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->calls(QStringLiteral("createBackground")), 2, 3000);
    bringToRunning(*pool, a);
    FakeHandle *h = handleFor(a);
    QCOMPARE(h->restoredAccess, QStringLiteral("a-token-new"));
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
    QTRY_VERIFY_WITH_TIMEOUT(!h->sendQueueEnabled, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(m_fake->log(h).count(QStringLiteral("stopSync")), 1, 3000);
    QVERIFY(pool->holdsStore(a));
    m_store.setWriteLatencyMs(0);
    QTRY_COMPARE_WITH_TIMEOUT(m_store.value(a, QStringLiteral("accessToken")),
                              QStringLiteral("a-rotated"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(h->sendQueueEnabled, 3000);
    pool->shutdownAsync([] {});
}

QTEST_GUILESS_MAIN(SessionPoolTest)

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
