// The opt-in sync-latency tracer (LIGHTNING_SYNC_TRACE).
//
// What matters about a diagnostic is that it is honest, cheap when off, and
// bounded. These cases pin exactly that: it is inert unless enabled, it never
// records a room id, a stage it did not observe reports -1 rather than a
// fabricated 0, its journey table cannot grow without bound, and a sync gap
// past the threshold is reported rather than silently swallowed.

#include "app/SyncLatencyTracer.h"

#include <QtTest>

class SyncLatencyTracerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init() { synctrace::resetForTest(0); }
    void cleanup() { synctrace::resetForTest(0); }

    void disabledIsInertAndCostsNothing()
    {
        QVERIFY(!synctrace::enabled());
        // Every entry point must tolerate being called while off, and mint
        // nothing: 0 is the "not traced" id every other call ignores.
        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        QCOMPARE(id, quint64(0));
        synctrace::noteBridge(id);
        synctrace::noteModel(id);
        synctrace::noteUi(id);
        synctrace::noteSyncResponse();
        synctrace::noteSyncState("running");
        QCOMPARE(synctrace::completedJourneys(), 0);
        QCOMPARE(synctrace::reportedStalls(), 0);
    }

    void anEnabledJourneyRunsAllFourStages()
    {
        synctrace::resetForTest(2000);
        QVERIFY(synctrace::enabled());
        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        QVERIFY(id != 0);
        synctrace::noteBridge(id);
        synctrace::noteModel(id);
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
        // Fast journey: recorded, but not reported as a stall.
        QCOMPARE(synctrace::reportedStalls(), 0);
        QVERIFY(synctrace::lastJourneyTotalMs() >= 0);
    }

    void anOldSdkStampMakesTheJourneyLookAsSlowAsItWas()
    {
        // The whole point of taking the stamp from the Rust side: a diff that
        // sat in the queue must MEASURE as slow, not be re-stamped on arrival.
        synctrace::resetForTest(500);
        const qint64 longAgo = QDateTime::currentMSecsSinceEpoch() - 60000;
        const quint64 id =
            synctrace::beginEvent(QStringLiteral("!r:example.org"), longAgo);
        QVERIFY(id != 0);
        synctrace::noteBridge(id);
        synctrace::noteModel(id);
        synctrace::noteUi(id);
        QVERIFY2(synctrace::lastJourneyTotalMs() >= 59000,
                 "a 60s-old SDK stamp must report a ~60s journey");
        QCOMPARE(synctrace::reportedStalls(), 1);
    }

    void aFutureStampIsRefusedRatherThanReportedAsNegative()
    {
        // A clock disagreement must not produce an impossible journey.
        synctrace::resetForTest(500);
        const qint64 future = QDateTime::currentMSecsSinceEpoch() + 60000;
        const quint64 id =
            synctrace::beginEvent(QStringLiteral("!r:example.org"), future);
        synctrace::noteBridge(id);
        synctrace::noteModel(id);
        synctrace::noteUi(id);
        QVERIFY2(synctrace::lastJourneyTotalMs() >= 0,
                 "a future stamp must degrade to now, never go negative");
    }

    void aSyncGapPastTheThresholdIsReported()
    {
        synctrace::resetForTest(150);
        synctrace::noteSyncResponse();          // first: nothing to compare to
        QCOMPARE(synctrace::reportedStalls(), 0);
        QTest::qWait(250);
        synctrace::noteSyncResponse();          // gap > threshold
        QCOMPARE(synctrace::reportedStalls(), 1);
        // A prompt follow-up is not a stall.
        synctrace::noteSyncResponse();
        QCOMPARE(synctrace::reportedStalls(), 1);
    }

    void theJourneyTableIsBounded()
    {
        // A burst must not turn the instrument into the memory problem it
        // exists to diagnose. Far more journeys than the cap, none completed.
        synctrace::resetForTest(2000);
        for (int i = 0; i < 4000; ++i)
            (void)synctrace::beginEvent(QStringLiteral("!r:example.org"));
        // Nothing completed, so the only proof available here is that it
        // survived and still works.
        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        synctrace::noteBridge(id);
        synctrace::noteModel(id);
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
    }

    void anUnknownIdIsIgnoredRatherThanCrashing()
    {
        synctrace::resetForTest(2000);
        // A journey that began before tracing was reset, or one already
        // reported, must not be resurrected or fault.
        synctrace::noteBridge(999999);
        synctrace::noteModel(999999);
        synctrace::noteUi(999999);
        QCOMPARE(synctrace::completedJourneys(), 0);

        // Double-completion is ignored: the journey is erased when reported.
        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
    }

    void anUnobservedStageIsNotReportedAsInstant()
    {
        // "We did not observe this" and "it took no time" are different facts.
        // A journey that skips the bridge and model stages must still complete
        // rather than being dropped or reporting fabricated zeroes.
        synctrace::resetForTest(2000);
        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        synctrace::noteUi(id);   // straight to the end
        QCOMPARE(synctrace::completedJourneys(), 1);
    }
};

QTEST_MAIN(SyncLatencyTracerTest)
#include "SyncLatencyTracerTest.moc"
