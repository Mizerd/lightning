
#include "models/TimelineScrollController.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr double kViewport = 900.0;
constexpr double kMinY = 0.0;
constexpr double kMaxY = 10000.0;
constexpr double kNotch = 120.0;
}

class TimelineScrollControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void defaultSpeedIsFastAndFartherThanStandard()
    {
        TimelineScrollController c;
        QCOMPARE(c.wheelSpeed(), TimelineScrollController::Fast);

        const double standard = c.notchDistanceForSpeed(
            TimelineScrollController::Standard, kViewport);
        const double fast = c.notchDistanceForSpeed(
            TimelineScrollController::Fast, kViewport);
        const double veryFast = c.notchDistanceForSpeed(
            TimelineScrollController::VeryFast, kViewport);

        QVERIFY(standard > 0.0);
        QVERIFY(fast > standard);
        QVERIFY(veryFast > fast);
        QVERIFY(fast >= 0.25 * kViewport);
    }

    void notchDistanceIsViewportRelativeAndBounded()
    {
        TimelineScrollController c;
        const double tall = c.notchDistance(2000.0);
        const double normal = c.notchDistance(900.0);
        const double tiny = c.notchDistance(120.0);
        QVERIFY(tall > normal);
        QVERIFY(tall <= 680.0);
        QVERIFY(tiny >= 130.0);
        QVERIFY(c.notchDistance(0.0) > 0.0);
    }

    void oneNotchMovesFullNotchDistance()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        const double up = c.wheelTargetY(+kNotch, start, kMinY, kMaxY, kViewport);
        QVERIFY(qFuzzyCompare(up, start - per));
    }

    void repeatedNotchesCoalesce()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        c.wheelTargetY(+kNotch, start, kMinY, kMaxY, kViewport);
        c.wheelTargetY(+kNotch, start - per, kMinY, kMaxY, kViewport);
        const double target = c.wheelTargetY(+kNotch, start - 2 * per,
                                             kMinY, kMaxY, kViewport);
        QVERIFY(qFuzzyCompare(target, start - 3 * per));
        QVERIFY(c.motionActive());
    }

    void partialAngleDeltasAccumulate()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        c.wheelTargetY(40.0, start, kMinY, kMaxY, kViewport);
        c.wheelTargetY(40.0, start - per / 3.0, kMinY, kMaxY, kViewport);
        const double target = c.wheelTargetY(40.0, start - 2.0 * per / 3.0,
                                             kMinY, kMaxY, kViewport);
        QVERIFY(qFuzzyCompare(target, start - per));
    }

    void oppositeDirectionRedirects()
    {
        TimelineScrollController c;
        const double per = c.notchDistance(kViewport);
        const double start = 5000.0;
        const double upTarget = c.wheelTargetY(+kNotch, start, kMinY, kMaxY,
                                               kViewport);
        QVERIFY(qFuzzyCompare(upTarget, start - per));
        const double live = 4950.0;
        const double downTarget = c.wheelTargetY(-kNotch, live, kMinY, kMaxY,
                                                 kViewport);
        QVERIFY(qFuzzyCompare(downTarget, live + per));
        QVERIFY(downTarget > live);
    }

    void pixelDeltaIsNotMultipliedLikeNotch()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double target = c.pixelTargetY(50.0, start, kMinY, kMaxY);
        QVERIFY(qFuzzyCompare(target, start - 50.0));
        const double per = c.notchDistance(kViewport);
        QVERIFY(qAbs(start - target) < 0.5 * per);
    }

    void pixelDeltaCancelsWheelMotion()
    {
        TimelineScrollController c;
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.pixelTargetY(30.0, 4900.0, kMinY, kMaxY);
        QVERIFY(!c.motionActive());
    }

    void clampsAtTopBound()
    {
        TimelineScrollController c;
        const double target = c.wheelTargetY(+10.0 * kNotch, 50.0,
                                             kMinY, kMaxY, kViewport);
        QCOMPARE(target, kMinY);
        const double px = c.pixelTargetY(9999.0, 50.0, kMinY, kMaxY);
        QCOMPARE(px, kMinY);
    }

    void clampsAtBottomBound()
    {
        TimelineScrollController c;
        const double target = c.wheelTargetY(-10.0 * kNotch, kMaxY - 50.0,
                                             kMinY, kMaxY, kViewport);
        QCOMPARE(target, kMaxY);
    }

    void shortContentPinsToTop()
    {
        TimelineScrollController c;
        const double up = c.wheelTargetY(+kNotch, 0.0, 0.0, -200.0,
                                         kViewport);
        QCOMPARE(up, 0.0);
        const double down = c.wheelTargetY(-kNotch, 0.0, 0.0, -200.0, kViewport);
        QCOMPARE(down, 0.0);
    }

    void cancelResetsCoalescing()
    {
        TimelineScrollController c;
        const double per = c.notchDistance(kViewport);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.cancel();
        QVERIFY(!c.motionActive());
        const double target = c.wheelTargetY(+kNotch, 4000.0, kMinY, kMaxY,
                                             kViewport);
        QVERIFY(qFuzzyCompare(target, 4000.0 - per));
    }

    void endMotionEmitsMotionActiveChange()
    {
        TimelineScrollController c;
        QSignalSpy spy(&c, &TimelineScrollController::motionActiveChanged);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QCOMPARE(spy.count(), 1);
        c.endMotion();
        QCOMPARE(spy.count(), 2);
    }

    void scrollTraceGateFollowsEnvironment()
    {
        qunsetenv("LIGHTNING_SCROLL_TRACE");
        TimelineScrollController off;
        QVERIFY(!off.scrollTraceEnabled());

        qputenv("LIGHTNING_SCROLL_TRACE", "1");
        TimelineScrollController on;
        QVERIFY(on.scrollTraceEnabled());
        qunsetenv("LIGHTNING_SCROLL_TRACE");
    }

    void invalidSpeedFallsBackToFast()
    {
        TimelineScrollController c;
        c.setWheelSpeedValue(-1);
        QCOMPARE(c.wheelSpeed(), TimelineScrollController::Fast);
        c.setWheelSpeedValue(99);
        QCOMPARE(c.wheelSpeed(), TimelineScrollController::Fast);
        c.setWheelSpeedValue(TimelineScrollController::VeryFast);
        QCOMPARE(c.wheelSpeed(), TimelineScrollController::VeryFast);
    }

    void speedAffectsDiscreteNotchDistance()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::Standard);
        const double stdTarget = c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY,
                                                kViewport);
        c.cancel();
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double vfTarget = c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY,
                                               kViewport);
        QVERIFY(vfTarget < stdTarget);
    }

    void speedDoesNotRescalePixelDelta()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::Standard);
        const double stdPx = c.pixelTargetY(50.0, 5000.0, kMinY, kMaxY);
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double vfPx = c.pixelTargetY(50.0, 5000.0, kMinY, kMaxY);
        QVERIFY(qFuzzyCompare(stdPx, vfPx));
        QVERIFY(qFuzzyCompare(stdPx, 5000.0 - 50.0));
    }

    void motionProgressesThroughIntermediatePositions()
    {
        TimelineScrollController c;
        QSignalSpy frames(&c, &TimelineScrollController::wheelPositionChanged);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        const double target = c.targetYForTest();
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(frames.count() >= 5);
        double prev = 5000.0;
        for (int i = 0; i < frames.count(); ++i) {
            const double y = frames.at(i).at(0).toDouble();
            QVERIFY2(y > prev - 0.001, "position must advance monotonically");
            QVERIFY2(y - prev < 0.6 * (target - 5000.0),
                     "one frame covered most of the notch — chunky");
            prev = y;
        }
        QVERIFY(qFuzzyCompare(prev, target));
    }

    void repeatedNotchesPreserveContinuousVelocity()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        double before = 0.0;
        for (int i = 0; i < 9; ++i) {
            const double y0 = c.positionYForTest();
            QVERIFY2(c.advanceMotion(16.0), "motion stopped between notches");
            before = c.positionYForTest() - y0;
            QVERIFY2(before > 0.0, "a frame produced no movement mid-gesture");
        }
        c.wheelNotch(-kNotch, c.positionYForTest(), kMinY, kMaxY, kViewport);
        const double y1 = c.positionYForTest();
        c.advanceMotion(16.0);
        const double after = c.positionYForTest() - y1;
        QVERIFY2(after >= before - 0.001,
                 "velocity dropped when a same-direction notch landed");
    }

    void reversalRedirectsOnNextFrame()
    {
        TimelineScrollController c;
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        c.advanceMotion(16.0);
        c.advanceMotion(16.0);
        const double mid = c.positionYForTest();
        c.wheelNotch(+kNotch, mid, kMinY, kMaxY, kViewport);
        c.advanceMotion(16.0);
        QVERIFY2(c.positionYForTest() < mid,
                 "reversal did not redirect immediately");
    }

    void motionSettlesWithinBoundedTimeAndStopsCleanly()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        QSignalSpy settled(&c, &TimelineScrollController::wheelMotionSettled);
        for (int i = 0; i < 5; ++i)
            c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        double elapsed = 0.0;
        while (c.motionActive() && elapsed < 5000.0) {
            c.advanceMotion(16.0);
            elapsed += 16.0;
        }
        QVERIFY2(elapsed < 2000.0, "motion did not settle in bounded time");
        QCOMPARE(settled.count(), 1);
        QVERIFY(!c.motionActive());
        QVERIFY(!c.tickerRunningForTest());
        QVERIFY(qFuzzyCompare(c.positionYForTest(), c.targetYForTest()));
    }

    void stalledFrameDoesNotJump()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        const double y0 = c.positionYForTest();
        c.advanceMotion(400.0);
        const double moved = c.positionYForTest() - y0;
        const double total = c.targetYForTest() - 5000.0;
        QVERIFY2(moved < 0.6 * total, "stalled frame jumped most of the way");
    }

    void boundReportSettlesMotion()
    {
        TimelineScrollController c;
        QSignalSpy settled(&c, &TimelineScrollController::wheelMotionSettled);
        c.wheelNotch(+kNotch, 100.0, kMinY, kMaxY, kViewport);
        c.advanceMotion(16.0);
        c.notifyBoundReached(kMinY);
        QVERIFY(!c.motionActive());
        QVERIFY(!c.tickerRunningForTest());
        QCOMPARE(settled.count(), 1);
        QCOMPARE(c.positionYForTest(), kMinY);
    }

    void translateActiveMotionPreservesRemainingDistance()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        for (int i = 0; i < 4; ++i)
            c.wheelNotch(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        for (int i = 0; i < 3; ++i)
            c.advanceMotion(16.0);
        QVERIFY(c.motionActive());
        const double remainingBefore = c.targetYForTest() - c.positionYForTest();

        constexpr double shift = 240.0;
        const double targetBefore = c.targetYForTest();
        c.translateActiveMotion(shift);

        QVERIFY(c.motionActive());
        QVERIFY(qFuzzyCompare(c.targetYForTest() - c.positionYForTest(),
                              remainingBefore));
        QVERIFY(qFuzzyCompare(c.targetYForTest(), targetBefore + shift));

        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(qFuzzyCompare(c.positionYForTest(), c.targetYForTest()));
        QVERIFY(qFuzzyCompare(c.positionYForTest(), targetBefore + shift));
    }

    void translateActiveMotionIsNoOpWhenNotActive()
    {
        TimelineScrollController c;
        QVERIFY(!c.motionActive());
        c.translateActiveMotion(500.0);
        QVERIFY(!c.motionActive());
        QCOMPARE(c.positionYForTest(), 0.0);
        QCOMPARE(c.targetYForTest(), 0.0);
    }

    void translateActiveMotionWithZeroDeltaChangesNothing()
    {
        TimelineScrollController c;
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        c.advanceMotion(16.0);
        const double position = c.positionYForTest();
        const double target = c.targetYForTest();
        c.translateActiveMotion(0.0);
        QCOMPARE(c.positionYForTest(), position);
        QCOMPARE(c.targetYForTest(), target);
    }

    void animateToDrivesSameEngine()
    {
        TimelineScrollController c;
        QSignalSpy frames(&c, &TimelineScrollController::wheelPositionChanged);
        c.animateTo(5800.0, 5000.0, kMinY, kMaxY);
        QVERIFY(c.motionActive());
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(frames.count() >= 4);
        QCOMPARE(c.positionYForTest(), 5800.0);
    }

    void cancelStopsEngineWithoutSettleSignal()
    {
        TimelineScrollController c;
        QSignalSpy frames(&c, &TimelineScrollController::wheelPositionChanged);
        QSignalSpy settled(&c, &TimelineScrollController::wheelMotionSettled);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        c.advanceMotion(16.0);
        const int framesBefore = frames.count();
        c.cancel();
        QVERIFY(!c.motionActive());
        QVERIFY(!c.tickerRunningForTest());
        QVERIFY(!c.advanceMotion(16.0));
        QCOMPARE(frames.count(), framesBefore);
        QCOMPARE(settled.count(), 0);
    }

    void pixelPathStopsEngine()
    {
        TimelineScrollController c;
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.tickerRunningForTest() || c.motionActive());
        const double y = c.pixelTargetY(25.0, 5100.0, kMinY, kMaxY);
        QVERIFY(qFuzzyCompare(y, 5075.0));
        QVERIFY(!c.motionActive());
        QVERIFY(!c.tickerRunningForTest());
    }

    void speedChangeDuringMotionIsSafe()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::Standard);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double perVf = c.notchDistance(kViewport);
        const double before = c.targetYForTest();
        const double after = c.wheelTargetY(+kNotch, 4000.0, kMinY, kMaxY,
                                            kViewport);
        QVERIFY(qFuzzyCompare(after, before - perVf));
    }

    void tallDelegateStillProducesSmoothFrames()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double tallViewport = 1600.0;
        QSignalSpy frames(&c, &TimelineScrollController::wheelPositionChanged);
        c.wheelNotch(-kNotch, 5000.0, kMinY, 100000.0, tallViewport);
        const double target = c.targetYForTest();
        const double total = target - 5000.0;
        QVERIFY(total > tallViewport * 0.3);
        double prev = 5000.0;
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(frames.count() >= 6);
        for (int i = 0; i < frames.count(); ++i) {
            const double y = frames.at(i).at(0).toDouble();
            QVERIFY(y > prev - 0.001);
            QVERIFY2(y - prev < 0.6 * total, "one frame covered most of it");
            prev = y;
        }
        QVERIFY(qFuzzyCompare(prev, target));
    }

    void slowCadenceNotchesStaySmooth()
    {
        TimelineScrollController c;
        double pos = 5000.0;
        for (int round = 0; round < 3; ++round) {
            c.wheelNotch(-kNotch, pos, kMinY, kMaxY, kViewport);
            const double total = c.targetYForTest() - pos;
            int frameCount = 0;
            double prev = pos;
            while (c.motionActive()) {
                c.advanceMotion(16.0);
                const double y = c.positionYForTest();
                QVERIFY2(y - prev < 0.6 * total, "chunky single-frame jump");
                prev = y;
                ++frameCount;
            }
            QVERIFY2(frameCount >= 4, "notch settled in too few frames (chunky)");
            pos = c.positionYForTest();
        }
    }

    void contentShrinkDuringMotionDoesNotOverscroll()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        c.wheelNotch(-kNotch, 5000.0, kMinY, 10000.0, kViewport);
        c.advanceMotion(16.0);
        const double target = c.targetYForTest();
        const double newMax = target - 40.0;
        c.notifyBoundReached(newMax);
        QVERIFY(!c.motionActive());
        QVERIFY(!c.tickerRunningForTest());
        QCOMPARE(c.positionYForTest(), newMax);
        QVERIFY(!c.advanceMotion(16.0));
        QVERIFY(c.positionYForTest() <= newMax);
    }

    void mainAndThreadControllersAreIndependent()
    {
        TimelineScrollController main;
        TimelineScrollController thread;

        main.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(main.motionActive());
        QVERIFY(!thread.motionActive());
        QVERIFY(!thread.tickerRunningForTest());

        main.advanceMotion(16.0);
        QCOMPARE(thread.positionYForTest(), 0.0);

        const double mainTarget = main.targetYForTest();
        thread.setWheelSpeed(TimelineScrollController::VeryFast);
        thread.wheelNotch(+kNotch, 800.0, kMinY, kMaxY, kViewport);
        QVERIFY(thread.motionActive());
        QCOMPARE(main.targetYForTest(), mainTarget);

        thread.cancel();
        QVERIFY(!thread.motionActive());
        QVERIFY(main.motionActive());
    }

    void positionsAndTargetsStayFinite()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double bigMax = 1.0e9;
        c.wheelNotch(-12.0 * kNotch, 5000.0, kMinY, bigMax, 2000.0);
        for (int i = 0; i < 60; ++i) {
            if (i == 15)
                c.wheelNotch(+8.0 * kNotch, c.positionYForTest(), kMinY,
                             bigMax, 2000.0);
            if (i == 30)
                c.advanceMotion(1000.0);
            else
                c.advanceMotion(16.0);
            QVERIFY(qIsFinite(c.positionYForTest()));
            QVERIFY(qIsFinite(c.targetYForTest()));
            QVERIFY(c.positionYForTest() >= kMinY - 0.001);
            QVERIFY(c.positionYForTest() <= bigMax + 0.001);
        }
        QVERIFY(qIsFinite(c.positionYForTest()));
        QVERIFY(qIsFinite(c.targetYForTest()));
    }

    void translateActiveMotionComposesAcrossRepeatedGrowthCorrections()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        for (int i = 0; i < 4; ++i)
            c.wheelNotch(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        for (int i = 0; i < 2; ++i)
            c.advanceMotion(16.0);
        const double remainingBefore =
            c.targetYForTest() - c.positionYForTest();
        const double targetBefore = c.targetYForTest();

        constexpr double firstGrowth = 270.0;
        c.translateActiveMotion(firstGrowth);
        QVERIFY(c.motionActive());
        QVERIFY(qFuzzyCompare(c.targetYForTest() - c.positionYForTest(),
                              remainingBefore));
        QVERIFY(qFuzzyCompare(c.targetYForTest(),
                              targetBefore + firstGrowth));

        c.advanceMotion(16.0);
        const double remainingMid = c.targetYForTest() - c.positionYForTest();
        constexpr double secondGrowth = 300.0;
        c.translateActiveMotion(secondGrowth);
        QVERIFY(c.motionActive());
        QVERIFY(qFuzzyCompare(c.targetYForTest() - c.positionYForTest(),
                              remainingMid));
        QVERIFY(qFuzzyCompare(c.targetYForTest(),
                              targetBefore + firstGrowth + secondGrowth));

        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(qFuzzyCompare(c.positionYForTest(), c.targetYForTest()));
        QVERIFY(qFuzzyCompare(c.positionYForTest(),
                              targetBefore + firstGrowth + secondGrowth));
    }

    void translateActiveMotionComposesDuringDownwardGlide()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::Fast);
        for (int i = 0; i < 3; ++i)
            c.wheelNotch(-kNotch, 500.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.advanceMotion(16.0);
        const double remainingBefore =
            c.targetYForTest() - c.positionYForTest();
        const double targetBefore = c.targetYForTest();

        constexpr double growth = 180.0;
        c.translateActiveMotion(growth);
        QVERIFY(qFuzzyCompare(c.targetYForTest() - c.positionYForTest(),
                              remainingBefore));
        QVERIFY(qFuzzyCompare(c.targetYForTest(), targetBefore + growth));
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(qFuzzyCompare(c.positionYForTest(), targetBefore + growth));
    }

    void anIncidentDumpNamesTheAnchorRowKindAndItsDistanceFromTheOldestRow()
    {
        TimelineScrollController c;
        c.setScrollRecorderEnabledForTest(true);
        c.recordScroll(QStringLiteral("anchor-restore"), 100, 2290, 0, 2290,
                       600, 3000, 40, 0, 0, QStringLiteral("$a:example.org"),
                       -800, QStringLiteral("divider"), 0);
        c.recordScroll(QStringLiteral("anchor-restore"), 2290, 2290, 0, 2290,
                       600, 3000, 40, 0, 0, QStringLiteral("$b:example.org"),
                       -10, QStringLiteral("event"), 3);
        c.recordScroll(QStringLiteral("wheel"), 2290, 2200, 0, 2290, 600,
                       3000, 40, 0, 0, QStringLiteral("$c:example.org"), 0,
                       QStringLiteral("secret body text"), 7);
        c.recordScroll(QStringLiteral("wheel"), 2200, 2100, 0, 2290, 600,
                       3000, 40, 0, 0, QString(), 0,
                       QStringLiteral("event"), 5);
        c.recordScroll(QStringLiteral("wheel"), 2100, 2000, 0, 2290, 600,
                       3000, 40, 0, 0, QStringLiteral("$d:example.org"), 0);
        QVERIFY(c.reportScrollIncident(QStringLiteral("oldest-bound"), 5));
        const QStringList lines =
            c.lastScrollIncidentDump().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 6);
        QVERIFY2(lines.at(1).endsWith(QStringLiteral(" kind=divider fromOldest=0")),
                 qPrintable(lines.at(1)));
        QVERIFY2(lines.at(2).endsWith(QStringLiteral(" kind=event fromOldest=3")),
                 qPrintable(lines.at(2)));
        QVERIFY2(lines.at(3).endsWith(QStringLiteral(" kind=- fromOldest=7")),
                 qPrintable(lines.at(3)));
        QVERIFY2(lines.at(4).endsWith(QStringLiteral(" kind=- fromOldest=-1")),
                 qPrintable(lines.at(4)));
        QVERIFY2(lines.at(5).endsWith(QStringLiteral(" kind=- fromOldest=-1")),
                 qPrintable(lines.at(5)));
        QVERIFY(!c.lastScrollIncidentDump().contains(QStringLiteral("secret")));
        QVERIFY(!c.lastScrollIncidentDump().contains(QStringLiteral("example.org")));
    }

    void theIncidentRecorderIsOptInBoundedAndCarriesNoIds()
    {
        qunsetenv("LIGHTNING_SCROLL_TRACE");
        qunsetenv("LIGHTNING_SCROLL_RECORDER");
        TimelineScrollController c;
        QVERIFY(!c.scrollRecorderEnabled());
        c.recordScroll(QStringLiteral("wheel"), 0, 10, 0, 100, 50, 500, 5, 0,
                       0, QStringLiteral("$secret-event:example.org"), 1);
        QCOMPARE(c.recordedScrollCount(), 0);
        QVERIFY(!c.reportScrollIncident(QStringLiteral("oldest-bound")));
        QCOMPARE(c.scrollIncidentCount(), 0);

        c.setScrollRecorderEnabledForTest(true);
        const int writes = TimelineScrollController::kRecorderCapacity + 300;
        for (int i = 0; i < writes; ++i) {
            c.recordScroll(i % 2 ? QStringLiteral("window-trim")
                                 : QStringLiteral("not-a-writer"),
                           i, i + 1, 0, 5000, 600, 9000, 400, 120, 3,
                           QStringLiteral("$secret-event-%1:example.org")
                               .arg(i % 7), -12.5);
        }
        QCOMPARE(c.recordedScrollCount(),
                 TimelineScrollController::kRecorderCapacity);
        QVERIFY(c.reportScrollIncident(QStringLiteral("oldest-bound\nX"), 5));
        QCOMPARE(c.scrollIncidentCount(), 1);
        const QString dump = c.lastScrollIncidentDump();
        QVERIFY(dump.startsWith(QStringLiteral("scroll incident code=oldest-bound ")));
        QCOMPARE(dump.count(QLatin1Char('\n')), 5);
        QVERIFY(dump.contains(QStringLiteral("w=window-trim")));
        QVERIFY(dump.contains(QStringLiteral("w=other")));
        QVERIFY(!dump.contains(QStringLiteral("secret")));
        QVERIFY(!dump.contains(QStringLiteral("example.org")));
        QVERIFY(dump.section(QLatin1Char('\n'), -1)
                    .contains(QStringLiteral("#%1 ").arg(writes)));
        QVERIFY(!c.reportScrollIncident(QStringLiteral("oldest-bound")));
        QCOMPARE(c.scrollIncidentCount(), 2);
    }
};

QTEST_MAIN(TimelineScrollControllerTest)
#include "TimelineScrollControllerTest.moc"
