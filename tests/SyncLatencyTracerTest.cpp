
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
        QCOMPARE(synctrace::reportedStalls(), 0);
        QVERIFY(synctrace::lastJourneyTotalMs() >= 0);
    }

    void anOldSdkStampMakesTheJourneyLookAsSlowAsItWas()
    {
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
        synctrace::noteSyncResponse();
        QCOMPARE(synctrace::reportedStalls(), 0);
        QTest::qWait(250);
        synctrace::noteSyncResponse();
        QCOMPARE(synctrace::reportedStalls(), 1);
        synctrace::noteSyncResponse();
        QCOMPARE(synctrace::reportedStalls(), 1);
    }

    void theJourneyTableIsBounded()
    {
        synctrace::resetForTest(2000);
        for (int i = 0; i < 4000; ++i)
            (void)synctrace::beginEvent(QStringLiteral("!r:example.org"));
        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        synctrace::noteBridge(id);
        synctrace::noteModel(id);
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
    }

    void anUnknownIdIsIgnoredRatherThanCrashing()
    {
        synctrace::resetForTest(2000);
        synctrace::noteBridge(999999);
        synctrace::noteModel(999999);
        synctrace::noteUi(999999);
        QCOMPARE(synctrace::completedJourneys(), 0);

        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
    }

    void anUnobservedStageIsNotReportedAsInstant()
    {
        synctrace::resetForTest(2000);
        const quint64 id = synctrace::beginEvent(QStringLiteral("!r:example.org"));
        synctrace::noteUi(id);
        QCOMPARE(synctrace::completedJourneys(), 1);
    }
};

QTEST_MAIN(SyncLatencyTracerTest)
#include "SyncLatencyTracerTest.moc"
