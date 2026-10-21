// Live accounts Phase 1: the keyring worker (contract §5.6, design §3.3.2,
// R1-M9, R2-M6) and the pause a locked keyring puts on background starts.
//
// One thread, per-account FIFO, 3 s reads and 10 s writes, never the GUI
// thread for a store that can block; a failed or late read is "locked", never
// "no account" (CLAUDE.md §6); a late answer is discarded; a locked keyring
// pauses starts and its readable edge resumes them.
//
// UNRUN when written (WP agents do not build); the lead runs it offscreen.
#include "accounts/KeyringWorker.h"
#include "accounts/RevocationProofs.h"
#include "accounts/SessionPool.h"
#include "accounts/SessionScheduler.h"
#include "accounts/SessionTokenCustody.h"
#include "accounts/StoreOwnershipRegistry.h"
#include "app/SettingsManager.h"
#include "auth/AccountManager.h"
#include "models/AccountActivityModel.h"
#include "storage/AppDataPaths.h"
#include "storage/InMemorySecretStore.h"
#include "storage/TwoBackendSecretStore.h"
#include "support/CountingSecretStore.h"
#include "support/FakeRustBridge.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>

using lightning::accounts::KeyringWorker;
using lightning::accounts::SessionPool;
using lightning::accounts::testing::CountingSecretStore;
using lightning::accounts::testing::FakeRustBridge;

namespace {

// A second client of the same keyring, as the worker's own libsecret
// instance is: every call goes to the shared store.
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

// Two keyrings a Flatpak's libsecret can land on (the sandbox file keyring
// and the Secret Service), shared by every store instance in the test the way
// the real ones are shared by every libsecret call in a process.
struct TwoKeyrings {
    QMutex mutex;
    QHash<QString, QString> file;
    QHash<QString, QString> service;
};

// The shipped routing (TwoBackendSecretStore) over TwoKeyrings, landed on
// `kind`. Thread-safe: the keyring worker's instance runs on its own thread.
class TwoKeyringStore final : public TwoBackendSecretStore
{
public:
    TwoKeyringStore(TwoKeyrings *keyrings, QString kind)
        : m_keyrings(keyrings), m_kind(std::move(kind)) {}
    bool isSecure() const override { return true; }
    bool isAvailable() const override { return true; }
    QString backendName() const override { return QStringLiteral("two fake keyrings"); }
    QString backendKind() const override { return m_kind; }

protected:
    QString readFrom(Backend backend, const QString &userId,
                     const QString &key) const override
    {
        QMutexLocker lock(&m_keyrings->mutex);
        m_lastReadFailed = false;
        return map(backend).value(userId + QLatin1Char('/') + key);
    }
    bool storeIn(Backend backend, const QString &userId, const QString &key,
                 const QString &value) override
    {
        QMutexLocker lock(&m_keyrings->mutex);
        map(backend).insert(userId + QLatin1Char('/') + key, value);
        return true;
    }
    bool clearFrom(Backend backend, const QString &userId, const QString *key) override
    {
        QMutexLocker lock(&m_keyrings->mutex);
        QHash<QString, QString> &m = map(backend);
        const QString prefix = userId + QLatin1Char('/');
        for (auto it = m.begin(); it != m.end();) {
            if (key ? it.key() == prefix + *key : it.key().startsWith(prefix))
                it = m.erase(it);
            else
                ++it;
        }
        return true;
    }

private:
    QHash<QString, QString> &map(Backend backend) const
    {
        const bool service = backend == Backend::Service
            || m_kind == QLatin1String(TwoBackendSecretStore::kServiceKind);
        return service ? m_keyrings->service : m_keyrings->file;
    }
    TwoKeyrings *m_keyrings;
    QString m_kind;
};

const QString kAlice = QStringLiteral("@alice:example.org");
const QString kBob = QStringLiteral("@bob:example.org");

} // namespace

class KeyringPauseTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void theTimeoutsAreThreeAndTenSeconds();
    void readsNeverRunOnTheGuiThread();
    void aFailedReadIsLockedNeverNoAccount();
    void aReadPastItsTimeoutIsLockedAndItsLateAnswerDiscarded();
    void writesGetTheLongerTimeout();
    void aHandbackLandsBeforeTheSameAccountsNextRead();
    void cancelForDropsQueuedJobsAndDiscardsTheRunningOne();
    void aFailedWriteIsReportedAndDeletesNothing();
    void processLocalStoresAreReadOnTheGuiThreadByAQueuedCall();
    void aBackendChangeRebuildsTheWorkersStoreAndSaysReadable();
    void quitFlushesQueuedWritesWithTheLoopBlocked();
    void aLockedKeyringPausesStartsUntilItIsReadable();
    void aCancelledWriteStillAnswersExactlyOnce();
    void aForbiddenAccountsWritesStopAndStayInFlightUntilTheStoreReturns();
    void aTimedOutWriteStaysInFlightUntilTheStoreReturns();
    void aHandbackAcceptsThisHostsOwnLiveWriteQueuedBeforeIt();
    void allowWritesLiftsAForbidOnlyOnceNothingOfTheOldSessionCanLand();
    // liveacc port onto 24a485c5 (356668cb for background accounts)
    void theWorkersStoreReadsAndWritesTheRecordedBackend();

private:
    std::unique_ptr<KeyringWorker> makeWorker(bool workerThread);
    void saveAccount(const QString &uid, const QString &device, const QString &token);

    QTemporaryDir m_config;
    CountingSecretStore m_store;
    std::unique_ptr<SettingsManager> m_settings;
    std::atomic<int> m_factoryCalls{0};
};

void KeyringPauseTest::initTestCase()
{
    QVERIFY(m_config.isValid());
    qputenv("XDG_CONFIG_HOME", m_config.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
    QCoreApplication::setApplicationName(QStringLiteral("keyring-pause-test"));
}

void KeyringPauseTest::init()
{
    m_settings.reset();
    {
        QSettings wipe;
        wipe.clear();
        wipe.sync();
    }
    m_store.setReadLatencyMs(0);
    m_store.setWriteLatencyMs(0);
    m_store.setFailReads(false);
    m_store.setFailWrites(false);
    m_store.resetCounts();
    m_factoryCalls = 0;
    m_settings = std::make_unique<SettingsManager>();
    m_settings->setSecretStore(&m_store);
    saveAccount(kAlice, QStringLiteral("ALICEDEV"), QStringLiteral("alice-old-token"));
    saveAccount(kBob, QStringLiteral("BOBDEV"), QStringLiteral("bob-token"));
    m_store.resetCounts();
}

void KeyringPauseTest::saveAccount(const QString &uid, const QString &device,
                                   const QString &token)
{
    m_settings->saveSession(QStringLiteral("https://example.org"), uid, device, token,
                            QStringLiteral("refresh-of-") + token);
}

std::unique_ptr<KeyringWorker> KeyringPauseTest::makeWorker(bool workerThread)
{
    KeyringWorker::Deps deps;
    deps.settings = m_settings.get();
    deps.guiSecretStore = &m_store;
    if (workerThread) {
        deps.secretStoreFactory = [this]() -> std::unique_ptr<SecretStore> {
            ++m_factoryCalls;
            return std::make_unique<ForwardingSecretStore>(&m_store);
        };
    }
    auto worker = std::make_unique<KeyringWorker>(std::move(deps));
    [&] { QCOMPARE(worker->mode(), workerThread ? KeyringWorker::Mode::WorkerThread
                                                : KeyringWorker::Mode::GuiQueued); }();
    return worker;
}

void KeyringPauseTest::theTimeoutsAreThreeAndTenSeconds()
{
    QCOMPARE(KeyringWorker::kReadTimeoutMs, 3000);
    QCOMPARE(KeyringWorker::kWriteTimeoutMs, 10000);
}

void KeyringPauseTest::readsNeverRunOnTheGuiThread()
{
    auto worker = makeWorker(true);
    KeyringWorker::ReadResult got;
    bool done = false;
    QThread *callbackThread = nullptr;
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &r) {
        got = r;
        done = true;
        callbackThread = QThread::currentThread();
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(got.ok);
    QVERIFY(!got.locked);
    QCOMPARE(got.accessToken, QStringLiteral("alice-old-token"));
    QCOMPARE(got.refreshToken, QStringLiteral("refresh-of-alice-old-token"));
    QCOMPARE(callbackThread, QCoreApplication::instance()->thread());
    const QList<QThread *> threads = m_store.callThreads();
    QVERIFY(!threads.isEmpty());
    for (QThread *t : threads)
        QVERIFY2(t != QCoreApplication::instance()->thread(), "a keyring read ran on the GUI thread");
    QCOMPARE(m_store.reads(kBob), 0);   // only the account asked for
}

void KeyringPauseTest::aFailedReadIsLockedNeverNoAccount()
{
    auto worker = makeWorker(true);
    QSignalSpy locked(worker.get(), &KeyringWorker::keyringLocked);
    m_store.setFailReads(true);
    KeyringWorker::ReadResult got;
    bool done = false;
    worker->read({kAlice, true, false}, [&](const KeyringWorker::ReadResult &r) {
        got = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(got.locked);
    QVERIFY(!got.ok);
    QVERIFY(got.accessToken.isEmpty());
    QCOMPARE(locked.size(), 1);
    QCOMPARE(locked.at(0).at(0).toString(), kAlice);
    QCOMPARE(m_store.deletes(kAlice), 0);
}

void KeyringPauseTest::aReadPastItsTimeoutIsLockedAndItsLateAnswerDiscarded()
{
    auto worker = makeWorker(true);
    worker->setTimeoutsForTest(200, 2000);
    m_store.setReadLatencyMs(700);
    QSignalSpy locked(worker.get(), &KeyringWorker::keyringLocked);
    QSignalSpy readableAgain(worker.get(), &KeyringWorker::keyringReadable);
    int answers = 0;
    KeyringWorker::ReadResult first;
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &r) {
        first = r;
        ++answers;
    });
    QTRY_COMPARE_WITH_TIMEOUT(answers, 1, 600);   // the timeout, not the store
    QCOMPARE(readableAgain.size(), 0);
    QVERIFY(first.locked);
    QVERIFY(first.timedOut);
    QVERIFY(worker->wedged());
    // While the store has not answered, a new job fails fast as locked.
    bool fast = false;
    worker->read({kBob, false, false}, [&](const KeyringWorker::ReadResult &r) {
        fast = r.locked && r.timedOut;
    });
    QTRY_VERIFY_WITH_TIMEOUT(fast, 300);
    // The late answer is dropped, and the worker recovers.
    m_store.setReadLatencyMs(0);
    QTRY_VERIFY_WITH_TIMEOUT(!worker->wedged(), 3000);
    QCOMPARE(answers, 1);
    // §18 round 1, MUST 4: what the wedge answered "locked" hears that the
    // keyring answers again (a paused start retries); before, only a backend
    // change said so. Mutation: drop the emit in finishRunning().
    QCOMPARE(readableAgain.size(), 1);
    bool readable = false;
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &r) {
        readable = r.ok && r.accessToken == QStringLiteral("alice-old-token");
    });
    QTRY_VERIFY_WITH_TIMEOUT(readable, 3000);
    QVERIFY(locked.size() >= 2);
}

void KeyringPauseTest::writesGetTheLongerTimeout()
{
    auto worker = makeWorker(true);
    worker->setTimeoutsForTest(150, 2000);
    m_store.setWriteLatencyMs(400);   // past the read timeout, inside the write one
    KeyringWorker::WriteResult got;
    bool done = false;
    KeyringWorker::WriteRequest request;
    request.userId = kAlice;
    request.deviceId = QStringLiteral("ALICEDEV");
    request.kind = KeyringWorker::WriteKind::Live;
    request.tokens = {QStringLiteral("alice-rotated"), QStringLiteral("alice-rotated-refresh")};
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        got = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(got.outcome, KeyringWorker::WriteOutcome::Written);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")), QStringLiteral("alice-rotated"));
    QCOMPARE(m_store.value(kAlice, QStringLiteral("refreshToken")),
             QStringLiteral("alice-rotated-refresh"));
}

// R1-C2: the per-account FIFO serialises "handback write before next read".
void KeyringPauseTest::aHandbackLandsBeforeTheSameAccountsNextRead()
{
    auto worker = makeWorker(true);
    m_store.setWriteLatencyMs(150);
    QStringList order;
    KeyringWorker::WriteRequest handback;
    handback.userId = kAlice;
    handback.deviceId = QStringLiteral("ALICEDEV");
    handback.kind = KeyringWorker::WriteKind::Handback;
    handback.tokens = {QStringLiteral("alice-final"), QStringLiteral("alice-final-refresh")};
    handback.expectedDigest =
        lightning::accounts::custody::accessTokenDigest(QStringLiteral("alice-old-token"));
    worker->write(handback, [&](const KeyringWorker::WriteResult &r) {
        order << (r.outcome == KeyringWorker::WriteOutcome::Written ? QStringLiteral("written")
                                                                    : QStringLiteral("write-failed"));
    });
    QString readBack;
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &r) {
        order << QStringLiteral("read");
        readBack = r.accessToken;
    });
    QTRY_COMPARE_WITH_TIMEOUT(order.size(), 2, 5000);
    QCOMPARE(order, (QStringList{QStringLiteral("written"), QStringLiteral("read")}));
    QCOMPARE(readBack, QStringLiteral("alice-final"));

    // A handback over a keyring someone else wrote since is skipped (CAS).
    bool skipped = false;
    worker->write(handback, [&](const KeyringWorker::WriteResult &r) {
        skipped = r.outcome == KeyringWorker::WriteOutcome::Skipped
            && r.handback == lightning::accounts::custody::Handback::SkippedKeyringChanged;
    });
    QTRY_VERIFY_WITH_TIMEOUT(skipped, 5000);
}

void KeyringPauseTest::cancelForDropsQueuedJobsAndDiscardsTheRunningOne()
{
    auto worker = makeWorker(true);
    m_store.setReadLatencyMs(200);
    int aliceAnswers = 0;
    bool bobAnswered = false;
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &) { ++aliceAnswers; });
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &) { ++aliceAnswers; });
    worker->read({kBob, false, false}, [&](const KeyringWorker::ReadResult &r) {
        bobAnswered = r.ok;
    });
    // Let the first Alice read start on the worker, then cancel.
    QTRY_VERIFY_WITH_TIMEOUT(m_store.reads(kAlice) > 0 || !m_store.callThreads().isEmpty(), 1000);
    worker->cancelFor(kAlice);
    QTRY_VERIFY_WITH_TIMEOUT(bobAnswered, 5000);
    QTest::qWait(300);
    QCOMPARE(aliceAnswers, 0);
}

void KeyringPauseTest::aFailedWriteIsReportedAndDeletesNothing()
{
    auto worker = makeWorker(true);
    m_store.setFailWrites(true);
    KeyringWorker::WriteResult got;
    bool done = false;
    KeyringWorker::WriteRequest request;
    request.userId = kAlice;
    request.deviceId = QStringLiteral("ALICEDEV");
    request.tokens = {QStringLiteral("alice-rotated"), QStringLiteral("r")};
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        got = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(got.outcome, KeyringWorker::WriteOutcome::Failed);
    QCOMPARE(m_store.deletes(kAlice), 0);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")), QStringLiteral("alice-old-token"));

    // A record naming another device is never this session's: skipped.
    m_store.setFailWrites(false);
    request.deviceId = QStringLiteral("SOMEONEELSE");
    done = false;
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        got = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(got.outcome, KeyringWorker::WriteOutcome::Skipped);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")), QStringLiteral("alice-old-token"));
}

void KeyringPauseTest::processLocalStoresAreReadOnTheGuiThreadByAQueuedCall()
{
    InMemorySecretStore memory;
    QCOMPARE(KeyringWorker::modeFor(&memory, true), KeyringWorker::Mode::GuiQueued);
    QCOMPARE(KeyringWorker::modeFor(&m_store, false), KeyringWorker::Mode::GuiQueued);
    QCOMPARE(KeyringWorker::modeFor(&m_store, true), KeyringWorker::Mode::WorkerThread);

    auto worker = makeWorker(false);
    bool done = false;
    QThread *ranOn = nullptr;
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &r) {
        done = r.ok;
        ranOn = QThread::currentThread();
    });
    // Never inside the caller's own stack (a drainer tick).
    QVERIFY(!done);
    QCOMPARE(m_store.reads(kAlice), 0);
    QTRY_VERIFY_WITH_TIMEOUT(done, 2000);
    QCOMPARE(ranOn, QCoreApplication::instance()->thread());
    for (QThread *t : m_store.callThreads())
        QCOMPARE(t, QCoreApplication::instance()->thread());
}

void KeyringPauseTest::aBackendChangeRebuildsTheWorkersStoreAndSaysReadable()
{
    auto worker = makeWorker(true);
    bool done = false;
    worker->read({kAlice, false, false}, [&](const KeyringWorker::ReadResult &) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(m_factoryCalls.load(), 1);
    QSignalSpy readable(worker.get(), &KeyringWorker::keyringReadable);
    Q_EMIT m_settings->secretBackendChanged();
    QCOMPARE(readable.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(m_factoryCalls.load(), 2, 3000);
}

void KeyringPauseTest::quitFlushesQueuedWritesWithTheLoopBlocked()
{
    auto worker = makeWorker(true);
    m_store.setWriteLatencyMs(50);
    int written = 0;
    for (const auto &[uid, device] : {std::pair{kAlice, QStringLiteral("ALICEDEV")},
                                      std::pair{kBob, QStringLiteral("BOBDEV")}}) {
        KeyringWorker::WriteRequest request;
        request.userId = uid;
        request.deviceId = device;
        request.tokens = {uid + QStringLiteral("-quit"), QString()};
        worker->write(request, [&](const KeyringWorker::WriteResult &r) {
            if (r.outcome == KeyringWorker::WriteOutcome::Written)
                ++written;
        });
    }
    // No event-loop turn: the queued pump never ran.
    QVERIFY(worker->waitForWrites(3000));
    QCOMPARE(written, 2);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")), kAlice + QStringLiteral("-quit"));
    QCOMPARE(m_store.value(kBob, QStringLiteral("accessToken")), kBob + QStringLiteral("-quit"));
    QCOMPARE(worker->pendingJobs(), 0);
}

// Design §6.1.6: a locked keyring pauses the start with "Keyring locked",
// creates nothing, marks no account gone; the readable edge resumes it.
void KeyringPauseTest::aLockedKeyringPausesStartsUntilItIsReadable()
{
    FakeRustBridge fake;
    AccountActivityModel activity;
    AccountManager accounts(m_settings.get());
    lightning::accounts::RevocationProofs proofs;
    lightning::accounts::StoreOwnershipRegistry registry;
    qint64 now = 0;
    SessionPool::Deps deps;
    deps.settings = m_settings.get();
    deps.accounts = &accounts;
    deps.activity = &activity;
    deps.proofs = &proofs;
    deps.registry = &registry;
    deps.api = &fake;
    deps.guiSecretStore = &m_store;
    deps.secretStoreFactory = [this]() -> std::unique_ptr<SecretStore> {
        return std::make_unique<ForwardingSecretStore>(&m_store);
    };
    deps.monotonicMs = [&now] { return now; };
    SessionPool pool(deps);
    QCOMPARE(m_settings->setKeepRunningFor(kBob, true), SettingsManager::KeepRunningResult::Ok);
    pool.setForegroundUserId(kAlice);
    m_store.setFailReads(true);
    pool.start();
    // The clock is scripted: the real-time lag probe would read a jump as a
    // suspend. And the fd limit is not under test here.
    pool.schedulerForTest()->stopProbe();
    pool.schedulerForTest()->setFdSoftLimitForTest(0);
    pool.noteForegroundInitialSyncDone();
    pool.admitNowForTest();
    QTRY_COMPARE_WITH_TIMEOUT(pool.liveState(kBob), AccountActivityModel::KeyringLocked, 3000);
    QCOMPARE(fake.calls(QStringLiteral("createBackground")), 0);
    QVERIFY(m_settings->hasSavedAccount(kBob));
    QCOMPARE(m_store.deletes(kBob), 0);
    // Still locked on the next admissions: no start, and no new reads either.
    const int readsWhileLocked = m_store.reads(kBob);
    now += 5000;
    pool.admitNowForTest();
    QCoreApplication::processEvents();
    QCOMPARE(fake.calls(QStringLiteral("createBackground")), 0);
    QCOMPARE(m_store.reads(kBob), readsWhileLocked);

    m_store.setFailReads(false);
    Q_EMIT m_settings->secretBackendChanged();
    now += 5000;
    QTRY_COMPARE_WITH_TIMEOUT(fake.calls(QStringLiteral("createBackground")), 1, 3000);
    pool.shutdownAsync([] {});
}

// Review M1: a retirement waits on its handback's callback to close the
// store's record and free its latch, so a cancelled WRITE still answers, once,
// and never inside cancelFor() itself. (A dropped read still never answers.)
void KeyringPauseTest::aCancelledWriteStillAnswersExactlyOnce()
{
    auto worker = makeWorker(true);
    m_store.setWriteLatencyMs(200);
    QList<KeyringWorker::WriteOutcome> outcomes;
    KeyringWorker::WriteRequest request;
    request.userId = kAlice;
    request.deviceId = QStringLiteral("ALICEDEV");
    request.kind = KeyringWorker::WriteKind::Live;
    request.tokens = {QStringLiteral("alice-rotated"), QStringLiteral("alice-rotated-refresh")};
    for (int i = 0; i < 2; ++i) {
        worker->write(request, [&](const KeyringWorker::WriteResult &r) {
            outcomes << r.outcome;
        });
    }
    // The first one is inside the store (its first item landed), the second
    // is queued behind it.
    QTRY_VERIFY_WITH_TIMEOUT(m_store.writes(kAlice) > 0, 3000);
    worker->cancelFor(kAlice);
    QVERIFY(outcomes.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(outcomes.size(), 2, 5000);
    QCOMPARE(outcomes.at(0), KeyringWorker::WriteOutcome::Cancelled);
    QCOMPARE(outcomes.at(1), KeyringWorker::WriteOutcome::Cancelled);
    QTest::qWait(300);
    QCOMPARE(outcomes.size(), 2);
    QCOMPARE(worker->pendingJobs(), 0);
    QVERIFY(!worker->writeInFlightFor(kAlice));
}

// Review M2: a removal must not report the store free while a token write of
// that account can still land: AppController deletes the secrets next, and a
// write after that is a token at rest with no record (B044). Once forbidden,
// a write running on the worker stops before its next item, the access token
// (which names the pair) never lands, writeInFlightFor() stays true until the
// store call returned, and every later write is Cancelled without reaching
// the store.
void KeyringPauseTest::aForbiddenAccountsWritesStopAndStayInFlightUntilTheStoreReturns()
{
    auto worker = makeWorker(true);
    m_store.setWriteLatencyMs(400);
    KeyringWorker::WriteOutcome outcome = KeyringWorker::WriteOutcome::Written;
    bool answered = false;
    KeyringWorker::WriteRequest request;
    request.userId = kAlice;
    request.deviceId = QStringLiteral("ALICEDEV");
    request.kind = KeyringWorker::WriteKind::Live;
    request.tokens = {QStringLiteral("alice-late"), QStringLiteral("alice-late-refresh")};
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        outcome = r.outcome;
        answered = true;
    });
    QVERIFY(worker->writeInFlightFor(kAlice));   // queued
    // On the worker now, inside its first store call (400 ms).
    QTRY_VERIFY_WITH_TIMEOUT(m_factoryCalls.load() == 1, 2000);
    QTest::qWait(100);
    QCOMPARE(m_store.writes(kAlice), 0);
    worker->forbidWritesFor(kAlice);
    QVERIFY2(worker->writeInFlightFor(kAlice), "the store is still inside the call");
    QTRY_VERIFY_WITH_TIMEOUT(!worker->writeInFlightFor(kAlice), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(answered, 2000);
    QCOMPARE(outcome, KeyringWorker::WriteOutcome::Cancelled);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")),
             QStringLiteral("alice-old-token"));
    // Later writes never reach the store.
    const int writesBefore = m_store.writes(kAlice);
    answered = false;
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        outcome = r.outcome;
        answered = true;
    });
    QVERIFY(!worker->writeInFlightFor(kAlice));
    QVERIFY(!answered);   // never inside write()
    QTRY_VERIFY_WITH_TIMEOUT(answered, 2000);
    QCOMPARE(outcome, KeyringWorker::WriteOutcome::Cancelled);
    QTest::qWait(500);
    QCOMPARE(m_store.writes(kAlice), writesBefore);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")),
             QStringLiteral("alice-old-token"));
    // Another account is untouched.
    m_store.setWriteLatencyMs(0);
    bool bobWritten = false;
    request.userId = kBob;
    request.deviceId = QStringLiteral("BOBDEV");
    request.tokens = {QStringLiteral("bob-rotated"), QStringLiteral("bob-rotated-refresh")};
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        bobWritten = r.outcome == KeyringWorker::WriteOutcome::Written;
    });
    QTRY_VERIFY_WITH_TIMEOUT(bobWritten, 3000);
}

// Review round 2: a removed account signed in again may write again, but
// never while a write of the forbidden session is still inside the store.
void KeyringPauseTest::allowWritesLiftsAForbidOnlyOnceNothingOfTheOldSessionCanLand()
{
    auto worker = makeWorker(true);
    m_store.setWriteLatencyMs(400);
    KeyringWorker::WriteRequest request;
    request.userId = kAlice;
    request.deviceId = QStringLiteral("ALICEDEV");
    request.kind = KeyringWorker::WriteKind::Live;
    request.tokens = {QStringLiteral("alice-old-session"), QStringLiteral("alice-old-refresh")};
    bool answered = false;
    worker->write(request, [&](const KeyringWorker::WriteResult &) { answered = true; });
    QTRY_VERIFY_WITH_TIMEOUT(m_factoryCalls.load() == 1, 2000);
    QTest::qWait(100);   // inside its store call
    worker->forbidWritesFor(kAlice);
    QVERIFY(worker->writeInFlightFor(kAlice));
    worker->allowWritesFor(kAlice);   // refused: the old write can still land
    QTRY_VERIFY_WITH_TIMEOUT(answered, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!worker->writeInFlightFor(kAlice), 5000);
    // Still forbidden.
    KeyringWorker::WriteOutcome outcome = KeyringWorker::WriteOutcome::Written;
    answered = false;
    request.tokens = {QStringLiteral("alice-too-early"), QStringLiteral("alice-too-early-r")};
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        outcome = r.outcome;
        answered = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(answered, 2000);
    QCOMPARE(outcome, KeyringWorker::WriteOutcome::Cancelled);
    QVERIFY(m_store.value(kAlice, QStringLiteral("accessToken"))
            != QStringLiteral("alice-too-early"));
    // Nothing in flight now: lifted, and the new session's write lands.
    m_store.setWriteLatencyMs(0);
    worker->allowWritesFor(kAlice);
    answered = false;
    request.tokens = {QStringLiteral("alice-new-session"), QStringLiteral("alice-new-refresh")};
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        outcome = r.outcome;
        answered = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(answered, 3000);
    QCOMPARE(outcome, KeyringWorker::WriteOutcome::Written);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")),
             QStringLiteral("alice-new-session"));
}

// A write that timed out is still inside the store: it can still land, so it
// is still in flight until the store answers.
void KeyringPauseTest::aTimedOutWriteStaysInFlightUntilTheStoreReturns()
{
    auto worker = makeWorker(true);
    worker->setTimeoutsForTest(100, 150);
    m_store.setWriteLatencyMs(400);
    KeyringWorker::WriteOutcome outcome = KeyringWorker::WriteOutcome::Written;
    bool answered = false;
    KeyringWorker::WriteRequest request;
    request.userId = kAlice;
    request.deviceId = QStringLiteral("ALICEDEV");
    request.tokens = {QStringLiteral("alice-slow"), QStringLiteral("alice-slow-refresh")};
    worker->write(request, [&](const KeyringWorker::WriteResult &r) {
        outcome = r.outcome;
        answered = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(answered, 2000);
    QCOMPARE(outcome, KeyringWorker::WriteOutcome::TimedOut);
    QVERIFY(worker->wedged());
    QVERIFY(worker->writeInFlightFor(kAlice));
    QVERIFY(!worker->writeInFlightFor(kBob));
    QTRY_VERIFY_WITH_TIMEOUT(!worker->wedged(), 5000);
    QVERIFY(!worker->writeInFlightFor(kAlice));
}

// Review L8: a live write still queued when the host retired lands just
// before the handback (per-account FIFO). The keyring then holds that pair,
// not the one the handback's compare-and-swap expected, and without being
// told it is this host's own the handback skipped the session's FINAL pair.
// Anything else in the keyring (a foreground sign-in) still refuses it.
void KeyringPauseTest::aHandbackAcceptsThisHostsOwnLiveWriteQueuedBeforeIt()
{
    using lightning::accounts::custody::accessTokenDigest;
    auto worker = makeWorker(true);
    KeyringWorker::WriteRequest live;
    live.userId = kAlice;
    live.deviceId = QStringLiteral("ALICEDEV");
    live.kind = KeyringWorker::WriteKind::Live;
    live.tokens = {QStringLiteral("alice-rotated-once"), QStringLiteral("r1")};
    KeyringWorker::WriteRequest handback;
    handback.userId = kAlice;
    handback.deviceId = QStringLiteral("ALICEDEV");
    handback.kind = KeyringWorker::WriteKind::Handback;
    handback.tokens = {QStringLiteral("alice-final"), QStringLiteral("r2")};
    // What the pool last saw persisted: the pair before the live write.
    handback.expectedDigest = accessTokenDigest(QStringLiteral("alice-old-token"));
    handback.acceptedDigests = {accessTokenDigest(QStringLiteral("alice-rotated-once"))};
    QList<KeyringWorker::WriteOutcome> outcomes;
    worker->write(live, [&](const KeyringWorker::WriteResult &r) { outcomes << r.outcome; });
    worker->write(handback, [&](const KeyringWorker::WriteResult &r) { outcomes << r.outcome; });
    QTRY_COMPARE_WITH_TIMEOUT(outcomes.size(), 2, 5000);
    QCOMPARE(outcomes.at(0), KeyringWorker::WriteOutcome::Written);
    QCOMPARE(outcomes.at(1), KeyringWorker::WriteOutcome::Written);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")), QStringLiteral("alice-final"));

    // A pair nobody named (the foreground signed in meanwhile) still refuses.
    m_store.setValue(kAlice, QStringLiteral("accessToken"), QStringLiteral("foreground-sign-in"));
    handback.tokens = {QStringLiteral("alice-final-2"), QStringLiteral("r3")};
    bool skipped = false;
    worker->write(handback, [&](const KeyringWorker::WriteResult &r) {
        skipped = r.outcome == KeyringWorker::WriteOutcome::Skipped;
    });
    QTRY_VERIFY_WITH_TIMEOUT(skipped, 5000);
    QCOMPARE(m_store.value(kAlice, QStringLiteral("accessToken")),
             QStringLiteral("foreground-sign-in"));
}

// 356668cb for a background account: a launch whose libsecret landed on the
// sandbox file keyring reads an account whose record says "service" in the
// Secret Service, as the GUI's store does. Before, the worker's own store
// (made by the factory, no lookup) read the file keyring, got a clean miss,
// and the pool showed "Needs sign-in" for a sign-in that is there. Writes go
// where the record says too. Mutation: drop setRecordedBackendLookup in
// KeyringWorkerRunner::ensureStore and the read comes back empty.
void KeyringPauseTest::theWorkersStoreReadsAndWritesTheRecordedBackend()
{
    TwoKeyrings keyrings;
    TwoKeyringStore gui(&keyrings, QString::fromLatin1(TwoBackendSecretStore::kFileKind));
    SettingsManager settings;
    settings.setSecretStore(&gui);
    const QString carol = QStringLiteral("@carol:example.org");
    settings.saveSession(QStringLiteral("https://example.org"), carol, QStringLiteral("CAROLDEV"),
                         QStringLiteral("carol-file-token"), QStringLiteral("carol-file-refresh"));
    // Carol's sign-in was last written by a launch on the Secret Service.
    {
        QMutexLocker lock(&keyrings.mutex);
        keyrings.file.clear();
        keyrings.service.insert(carol + QStringLiteral("/accessToken"),
                                QStringLiteral("carol-service-token"));
        keyrings.service.insert(carol + QStringLiteral("/refreshToken"),
                                QStringLiteral("carol-service-refresh"));
    }
    {
        QSettings raw;
        raw.setValue(QStringLiteral("accounts/") + matrix::app_data::safeUserSlug(carol)
                         + QStringLiteral("/keyringBackend"),
                     QString::fromLatin1(TwoBackendSecretStore::kServiceKind));
        raw.sync();
    }
    QCOMPARE(settings.keyringBackendFor(carol),
             QString::fromLatin1(TwoBackendSecretStore::kServiceKind));
    // The GUI's own store already routes there (the main-side behaviour).
    QCOMPARE(settings.accessTokenFor(carol), QStringLiteral("carol-service-token"));

    KeyringWorker::Deps deps;
    deps.settings = &settings;
    deps.guiSecretStore = &gui;
    deps.secretStoreFactory = [&keyrings]() -> std::unique_ptr<SecretStore> {
        return std::make_unique<TwoKeyringStore>(
            &keyrings, QString::fromLatin1(TwoBackendSecretStore::kFileKind));
    };
    KeyringWorker worker(std::move(deps));
    QCOMPARE(worker.mode(), KeyringWorker::Mode::WorkerThread);

    KeyringWorker::ReadResult got;
    bool done = false;
    worker.read({carol, false, false}, [&](const KeyringWorker::ReadResult &r) {
        got = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(got.ok);
    QCOMPARE(got.accessToken, QStringLiteral("carol-service-token"));
    QCOMPARE(got.refreshToken, QStringLiteral("carol-service-refresh"));

    // A live write of a rotation lands beside it, not in the file keyring.
    KeyringWorker::WriteRequest write;
    write.userId = carol;
    write.deviceId = QStringLiteral("CAROLDEV");
    write.kind = KeyringWorker::WriteKind::Live;
    write.tokens = lightning::accounts::SessionTokenPair{QStringLiteral("carol-rotated"),
                                                         QStringLiteral("carol-rotated-refresh")};
    KeyringWorker::WriteResult wrote;
    done = false;
    worker.write(write, [&](const KeyringWorker::WriteResult &r) {
        wrote = r;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(int(wrote.outcome), int(KeyringWorker::WriteOutcome::Written));
    QMutexLocker lock(&keyrings.mutex);
    QCOMPARE(keyrings.service.value(carol + QStringLiteral("/accessToken")),
             QStringLiteral("carol-rotated"));
    QVERIFY(keyrings.file.value(carol + QStringLiteral("/accessToken")).isEmpty());
}

QTEST_GUILESS_MAIN(KeyringPauseTest)
#include "KeyringPauseTest.moc"
