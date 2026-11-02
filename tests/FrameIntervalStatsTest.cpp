#include "app/FrameIntervalProbe.h"

#include <QtTest/QtTest>

class FrameIntervalStatsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void percentilesAreNearestRankAndTheWorstFrameIsKept()
    {
        FrameIntervalStats s;
        QCOMPARE(s.percentile(50), 0.0);
        QCOMPARE(s.worst(), 0.0);
        for (int i = 0; i < 98; ++i)
            s.add(16.7);
        s.add(40.0);
        s.add(120.0);
        QCOMPARE(s.count(), 100);
        QCOMPARE(s.percentile(50), 16.7);
        QCOMPARE(s.percentile(95), 16.7);
        QCOMPARE(s.percentile(99), 40.0);
        QCOMPARE(s.percentile(100), 120.0);
        QCOMPARE(s.worst(), 120.0);
        QCOMPARE(s.countOver(1000.0 / 60 * 1.25), 2);
        QCOMPARE(s.countOver(1000.0 / 60 * 2.0), 2);
        const QString line = s.summary(60.0, 10.0);
        QVERIFY(line.startsWith(QStringLiteral("frame-intervals n=100 ")));
        QVERIFY(line.contains(QStringLiteral("p99=40.00")));
        QVERIFY(line.contains(QStringLiteral("max=120.00")));
        QVERIFY(line.contains(QStringLiteral("over2x=2")));
        QVERIFY(s.summary(0.0, 10.0).contains(QStringLiteral("refresh_hz=60.0")));
    }

    void aSlowPeriodicRepaintIsNotMotionButAStallingScrollIs()
    {
        FrameIntervalStats idle;
        for (int i = 0; i < 50; ++i)
            idle.add(200.0);
        QVERIFY(!idle.continuousMotion(60.0));

        FrameIntervalStats scroll;
        for (int i = 0; i < 90; ++i)
            scroll.add(16.7);
        for (int i = 0; i < 10; ++i)
            scroll.add(140.0);
        QVERIFY(scroll.continuousMotion(60.0));
        QCOMPARE(scroll.countOver(1000.0 / 60 * 2.0), 10);
    }

    void aHigherRefreshRateTightensTheBudget()
    {
        FrameIntervalStats s;
        for (int i = 0; i < 10; ++i)
            s.add(12.0);
        QCOMPARE(s.countOver(1000.0 / 60 * 1.25), 0);
        QCOMPARE(s.countOver(1000.0 / 120 * 1.25), 10);
    }
};

QTEST_MAIN(FrameIntervalStatsTest)
#include "FrameIntervalStatsTest.moc"
