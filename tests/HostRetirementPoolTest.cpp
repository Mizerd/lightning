#include "accounts/HostRetirementPool.h"
#include "support/FakeRustBridge.h"
#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QtTest>

#include <memory>

using lightning::accounts::HostRetirementPool;
using lightning::accounts::HostSignInRetirements;
using lightning::accounts::testing::FakeHandle;
using lightning::accounts::testing::FakeRustBridge;

class HostRetirementPoolTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void threadsAreLiveHostsPlusOneCappedAtSix();
    void retirementsRunInParallelOffTheGuiThread();
    void persistReadsFinalTokensBetweenShutdownAndDestroy();
    void anUnconfirmedCloseIsReported();
    void aSlowHostRetirementLeavesTheForegroundPoolAlone();
    void quitDeliversFinishedJobsWithoutTheEventLoop();
    void resultsAreDroppedWhenThePoolIsGone();
    void hostSignInRetirementsCountPerAccount();
    void aDestroyNeverOvertakesABlockingCallOnTheSameHandle();

private:
    static void *newHandle(FakeRustBridge &fake, const QString &path)
    {
        return fake.createBackground(path, QByteArrayLiteral("{\"v\":1}"));
    }
};

void HostRetirementPoolTest::threadsAreLiveHostsPlusOneCappedAtSix()
{
    FakeRustBridge fake;
    HostRetirementPool pool(&fake);
    pool.setLiveHostCount(0);
    QCOMPARE(pool.maxThreadCount(), 1);
    pool.setLiveHostCount(1);
    QCOMPARE(pool.maxThreadCount(), 2);
    pool.setLiveHostCount(4);
    QCOMPARE(pool.maxThreadCount(), 5);
    pool.setLiveHostCount(5);
    QCOMPARE(pool.maxThreadCount(), 6);
    pool.setLiveHostCount(40);
    QCOMPARE(pool.maxThreadCount(), HostRetirementPool::kMaxThreads);
}

void HostRetirementPoolTest::retirementsRunInParallelOffTheGuiThread()
{
    FakeRustBridge fake;
    fake.setRetireDelayMs(500);
    HostRetirementPool pool(&fake);
    pool.setLiveHostCount(2);
    void *a = newHandle(fake, QStringLiteral("/stores/a"));
    void *b = newHandle(fake, QStringLiteral("/stores/b"));
    int done = 0;
    QList<QThread *> callbackThreads;
    for (void *h : {a, b}) {
        pool.retire(HostRetirementPool::RetireJob{h, QStringLiteral("slug"), false},
                    [&](const HostRetirementPool::RetireOutcome &outcome) {
            QVERIFY(outcome.storeClosed);
            callbackThreads.append(QThread::currentThread());
            ++done;
        });
    }
    QCOMPARE(pool.activeJobs(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(done, 2, 10000);
    QCOMPARE(fake.maxConcurrentBlocking(), 2);
    for (QThread *t : callbackThreads)
        QCOMPARE(t, QCoreApplication::instance()->thread());
    const QList<QThread *> blocking = fake.blockingThreads();
    QVERIFY(!blocking.isEmpty());
    for (QThread *t : blocking)
        QVERIFY2(t != QCoreApplication::instance()->thread(), "a blocking FFI ran on the GUI thread");
    QCOMPARE(pool.activeJobs(), 0);
}

void HostRetirementPoolTest::persistReadsFinalTokensBetweenShutdownAndDestroy()
{
    FakeRustBridge fake;
    HostRetirementPool pool(&fake);
    auto *kept = static_cast<FakeHandle *>(newHandle(fake, QStringLiteral("/stores/kept")));
    fake.setFinalTokens(kept, QStringLiteral("final-access-fixture"),
                        QStringLiteral("final-refresh-fixture"));
    HostRetirementPool::RetireOutcome got;
    bool ran = false;
    pool.retire(HostRetirementPool::RetireJob{kept, QStringLiteral("k"), true},
                [&](const HostRetirementPool::RetireOutcome &o) {
        got = o;
        ran = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(ran, 5000);
    QCOMPARE(fake.log(kept), (QStringList{QStringLiteral("createBackground"),
                                          QStringLiteral("shutdownTasks"),
                                          QStringLiteral("finalSessionTokens"),
                                          QStringLiteral("tokenLedgerRetain"),
                                          QStringLiteral("destroyAndReport"),
                                          QStringLiteral("tokenLedgerLatest")}));
    QVERIFY(got.collectedFinalTokens);
    QCOMPARE(got.finalTokens.accessToken, QStringLiteral("final-access-fixture"));
    QCOMPARE(got.finalTokens.refreshToken, QStringLiteral("final-refresh-fixture"));
    QVERIFY(!got.rotatedWhileClosing);
    QCOMPARE(fake.ledgerRetains(kept), 0);

    auto *late = static_cast<FakeHandle *>(newHandle(fake, QStringLiteral("/stores/late")));
    fake.setFinalTokens(late, QStringLiteral("before-close-access"),
                        QStringLiteral("before-close-refresh"));
    fake.setLedgerLatest(late, QStringLiteral("rotated-while-closing-access"),
                         QStringLiteral("rotated-while-closing-refresh"));
    ran = false;
    pool.retire(HostRetirementPool::RetireJob{late, QStringLiteral("l"), true},
                [&](const HostRetirementPool::RetireOutcome &o) {
        got = o;
        ran = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(ran, 5000);
    QVERIFY(got.rotatedWhileClosing);
    QCOMPARE(got.finalTokens.accessToken, QStringLiteral("rotated-while-closing-access"));
    QCOMPARE(got.finalTokens.refreshToken, QStringLiteral("rotated-while-closing-refresh"));
    QCOMPARE(fake.ledgerRetains(late), 0);

    auto *dropped = static_cast<FakeHandle *>(newHandle(fake, QStringLiteral("/stores/dropped")));
    ran = false;
    pool.retire(HostRetirementPool::RetireJob{dropped, QStringLiteral("d"), false},
                [&](const HostRetirementPool::RetireOutcome &o) {
        got = o;
        ran = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(ran, 5000);
    QVERIFY(!fake.log(dropped).contains(QStringLiteral("finalSessionTokens")));
    QVERIFY(!got.collectedFinalTokens);
    QVERIFY(got.finalTokens.accessToken.isEmpty());
}

void HostRetirementPoolTest::anUnconfirmedCloseIsReported()
{
    FakeRustBridge fake;
    fake.setDestroyReport(QStringLiteral("store_closed=false waited_ms=5000 builds=1 "
                                         "client_alive=true still_open=crypto "
                                         "runtime_shutdown=timed_out"));
    HostRetirementPool pool(&fake);
    bool closed = true;
    bool ran = false;
    pool.retire(HostRetirementPool::RetireJob{newHandle(fake, QStringLiteral("/stores/held")),
                                              QStringLiteral("h"), false},
                [&](const HostRetirementPool::RetireOutcome &o) {
        closed = o.storeClosed;
        ran = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(ran, 5000);
    QVERIFY(!closed);
}

void HostRetirementPoolTest::aSlowHostRetirementLeavesTheForegroundPoolAlone()
{
    FakeRustBridge fake;
    fake.setRetireDelayMs(1500);
    HostRetirementPool pool(&fake);
    bool ran = false;
    pool.retire(HostRetirementPool::RetireJob{newHandle(fake, QStringLiteral("/stores/slow")),
                                              QStringLiteral("s"), true},
                [&](const HostRetirementPool::RetireOutcome &) { ran = true; });
    QTRY_VERIFY_WITH_TIMEOUT(!fake.blockingThreads().isEmpty(), 2000);
    QCOMPARE(QThreadPool::globalInstance()->activeThreadCount(), 0);
#ifdef ENABLE_RUST_SDK_BACKEND
    QElapsedTimer waited;
    waited.start();
    QVERIFY(RustSdkMatrixClient::waitForRustRetirement(0));
    QVERIFY(waited.elapsed() < 750);
#endif
    QVERIFY(!ran);
    QTRY_VERIFY_WITH_TIMEOUT(ran, 10000);
}

void HostRetirementPoolTest::quitDeliversFinishedJobsWithoutTheEventLoop()
{
    FakeRustBridge fake;
    HostRetirementPool pool(&fake);
    int runs = 0;
    pool.retire(HostRetirementPool::RetireJob{newHandle(fake, QStringLiteral("/stores/quit")),
                                              QStringLiteral("q"), true},
                [&](const HostRetirementPool::RetireOutcome &) { ++runs; });
    QVERIFY(pool.waitForDone(5000));
    QCOMPARE(runs, 0);
    pool.deliverFinishedNow();
    QCOMPARE(runs, 1);
    QCOMPARE(pool.activeJobs(), 0);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCOMPARE(runs, 1);
}

void HostRetirementPoolTest::resultsAreDroppedWhenThePoolIsGone()
{
    FakeRustBridge fake;
    fake.setRetireDelayMs(100);
    int runs = 0;
    {
        auto pool = std::make_unique<HostRetirementPool>(&fake);
        pool->retire(HostRetirementPool::RetireJob{newHandle(fake, QStringLiteral("/stores/gone")),
                                                   QStringLiteral("g"), false},
                     [&](const HostRetirementPool::RetireOutcome &) { ++runs; });
    }
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QTest::qWait(50);
    QCOMPARE(runs, 0);
}

void HostRetirementPoolTest::hostSignInRetirementsCountPerAccount()
{
    HostSignInRetirements &r = HostSignInRetirements::process();
    r.clearForTest();
    const QString a = QStringLiteral("@a:example.org");
    const QString b = QStringLiteral("@b:example.org");
    r.begin(a);
    r.begin(a);
    QVERIFY(r.contains(a));
    QVERIFY(!r.contains(b));
    QCOMPARE(r.count(a), 2);
    r.end(a);
    QVERIFY2(r.contains(a), "one of two retirements of the account still pending");
    r.end(a);
    QVERIFY(!r.contains(a));
    r.end(b);
    QVERIFY(!r.contains(b));
    r.begin(QString());
    QVERIFY(!r.contains(QString()));
}

void HostRetirementPoolTest::aDestroyNeverOvertakesABlockingCallOnTheSameHandle()
{
    FakeRustBridge fake;
    fake.setStopSyncDelayMs(1500);
    fake.setLogoutDelayMs(1500);
    HostRetirementPool pool(&fake);
    pool.setLiveHostCount(4);
    auto *stopped = static_cast<FakeHandle *>(newHandle(fake, QStringLiteral("/stores/stopped")));
    auto *signedOut = static_cast<FakeHandle *>(newHandle(fake, QStringLiteral("/stores/signed-out")));
    auto *other = static_cast<FakeHandle *>(newHandle(fake, QStringLiteral("/stores/other")));
    QStringList gui;
    pool.runBlocking(stopped, [&fake, stopped] { fake.stopSync(stopped); },
                     [&gui] { gui << QStringLiteral("stop"); });
    pool.signOut(signedOut, false, [&gui](qint64) { gui << QStringLiteral("logout"); });
    int retired = 0;
    for (FakeHandle *h : {stopped, signedOut, other}) {
        pool.retire(HostRetirementPool::RetireJob{h, QStringLiteral("h"), false},
                    [&](const HostRetirementPool::RetireOutcome &) { ++retired; });
    }
    const QString destroy = QStringLiteral("destroyAndReport");
    QTRY_VERIFY_WITH_TIMEOUT(fake.log(other).contains(destroy), 1000);
    QVERIFY(!fake.log(stopped).contains(destroy));
    QVERIFY(!fake.log(signedOut).contains(destroy));
    QTRY_COMPARE_WITH_TIMEOUT(retired, 3, 10000);
    QCOMPARE(fake.destroyOverlaps(), 0);
    QCOMPARE(fake.callsAfterDestroy(), 0);
    QCOMPARE(fake.log(stopped), (QStringList{QStringLiteral("createBackground"),
                                             QStringLiteral("stopSync"),
                                             QStringLiteral("shutdownTasks"),
                                             QStringLiteral("destroyAndReport")}));
    QCOMPARE(fake.log(signedOut), (QStringList{QStringLiteral("createBackground"),
                                               QStringLiteral("logout"),
                                               QStringLiteral("shutdownTasks"),
                                               QStringLiteral("destroyAndReport")}));
    QCOMPARE(gui.size(), 2);
    QCOMPARE(pool.activeJobs(), 0);
}

QTEST_GUILESS_MAIN(HostRetirementPoolTest)
#include "HostRetirementPoolTest.moc"
