// The microphone meter's scale and update rate (src/calls/AudioLevelMeter.h),
// shared by the in-call meter and the Settings microphone test.

#include <QtTest/QtTest>

#include <cmath>
#include <limits>

#include "calls/AudioLevelMeter.h"

using lightning::calls::kMeterFloorDb;
using lightning::calls::kMeterMinIntervalMs;
using lightning::calls::meterFraction;
using lightning::calls::MeterThrottle;

class AudioLevelMeterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // -350 is `level`'s reading for digital silence, not an error: it must
    // draw an empty bar, as must anything at or below the floor and NaN.
    void silenceAndGarbageDrawAnEmptyBar()
    {
        QCOMPARE(meterFraction(-350.0), 0.0);
        QCOMPARE(meterFraction(kMeterFloorDb), 0.0);
        QCOMPARE(meterFraction(-1000.0), 0.0);
        QCOMPARE(meterFraction(std::numeric_limits<double>::quiet_NaN()), 0.0);
        QCOMPARE(meterFraction(-std::numeric_limits<double>::infinity()), 0.0);
    }

    // Linear in dB between the floor and full scale.
    void theScaleIsLinearInDecibels()
    {
        QCOMPARE(meterFraction(0.0), 1.0);
        QCOMPARE(meterFraction(6.0), 1.0);
        QCOMPARE(meterFraction(kMeterFloorDb / 2.0), 0.5);
        QVERIFY(meterFraction(kMeterFloorDb + 0.1) > 0.0);
        QVERIFY(meterFraction(-20.0) > meterFraction(-40.0));
    }

    // A reading every 10 ms for a second (a backlog delivered at once, or a
    // faster element) publishes at most once per kMeterMinIntervalMs.
    void readingsArePublishedAtMostOncePerInterval()
    {
        MeterThrottle throttle;
        int published = 0;
        for (qint64 t = 0; t < 1000; t += 10) {
            double shown = 0.0;
            if (throttle.offer(-20.0, t, &shown))
                ++published;
        }
        QVERIFY2(published <= 1000 / kMeterMinIntervalMs + 1,
                 qPrintable(QString::number(published)));
        // And it does publish: a throttle that swallowed everything would
        // pass the bound above.
        QVERIFY2(published >= 1000 / kMeterMinIntervalMs - 1,
                 qPrintable(QString::number(published)));
    }

    // The capture's own rate (one reading per 50 ms) is never thinned.
    void theCaptureRateIsNeverThinned()
    {
        MeterThrottle throttle;
        int published = 0;
        for (qint64 t = 0; t < 1000; t += 50) {
            if (throttle.offer(-20.0, t, nullptr))
                ++published;
        }
        QCOMPARE(published, 20);
    }

    // A word that falls between two publications is not lost: the loudest
    // held reading goes out with the next one.
    void aPeakBetweenPublicationsIsCarriedToTheNext()
    {
        MeterThrottle throttle;
        double shown = 0.0;
        QVERIFY(throttle.offer(-50.0, 0, &shown));
        QCOMPARE(shown, -50.0);
        QVERIFY(!throttle.offer(-6.0, 10, &shown));
        QVERIFY(!throttle.offer(-45.0, 20, &shown));
        QVERIFY(throttle.offer(-40.0, kMeterMinIntervalMs, &shown));
        QCOMPARE(shown, -6.0);
        // Released: the next window starts from what it hears.
        QVERIFY(throttle.offer(-30.0, 2 * kMeterMinIntervalMs, &shown));
        QCOMPARE(shown, -30.0);
    }

    // A clock that steps backwards publishes rather than freezing the bar.
    void aClockStepBackwardsDoesNotFreezeTheMeter()
    {
        MeterThrottle throttle;
        QVERIFY(throttle.offer(-20.0, 5000, nullptr));
        QVERIFY(throttle.offer(-20.0, 100, nullptr));
    }

    void resetStartsFresh()
    {
        MeterThrottle throttle;
        double shown = 0.0;
        QVERIFY(throttle.offer(-3.0, 0, &shown));
        QVERIFY(!throttle.offer(-1.0, 5, &shown));
        throttle.reset();
        // Neither the rate history nor the held -1 survives.
        QVERIFY(throttle.offer(-30.0, 6, &shown));
        QCOMPARE(shown, -30.0);
    }
};

QTEST_GUILESS_MAIN(AudioLevelMeterTest)
#include "AudioLevelMeterTest.moc"
