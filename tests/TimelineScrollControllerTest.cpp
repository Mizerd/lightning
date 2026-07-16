// v0.5.19: deterministic tests for the device-aware timeline wheel-scroll
// policy. The offscreen QPA platform used by the QML tests never incubates
// ListView delegates, so geometry-dependent scroll behaviour cannot be driven
// through the real view. The scroll MATH therefore lives in
// TimelineScrollController and is exercised here in isolation: per-notch
// distance and ordering, coalescing, partial-delta accumulation, direction
// reversal, the pixel-delta vs angle-delta distinction, bound clamping, and
// motion cancellation.

#include "models/TimelineScrollController.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
constexpr double kViewport = 900.0;   // a normal timeline viewport height
constexpr double kMinY = 0.0;
constexpr double kMaxY = 10000.0;     // a long room
constexpr double kNotch = 120.0;      // one physical wheel notch (angleDelta.y)
}

class TimelineScrollControllerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The new default must be Fast, and a single physical notch must move
    // farther than the modest Standard (≈ 0.5.18) distance.
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

        // Strictly ordered distances.
        QVERIFY(standard > 0.0);
        QVERIFY(fast > standard);
        QVERIFY(veryFast > fast);
        // The default (Fast) notch is a meaningful fraction of the viewport —
        // several message lines, not the couple of lines 0.5.18 produced.
        QVERIFY(fast >= 0.25 * kViewport);
    }

    // Per-notch distance scales with the viewport but stays within absolute
    // bounds — never hardcoded to one display size.
    void notchDistanceIsViewportRelativeAndBounded()
    {
        TimelineScrollController c;
        const double tall = c.notchDistance(2000.0);
        const double normal = c.notchDistance(900.0);
        const double tiny = c.notchDistance(120.0);
        QVERIFY(tall > normal);       // taller viewport → farther notch …
        QVERIFY(tall <= 680.0);       // … but clamped at the Fast maximum.
        QVERIFY(tiny >= 130.0);       // tiny viewport still moves usefully.
        // A non-positive viewport falls back to a sane distance, never 0.
        QVERIFY(c.notchDistance(0.0) > 0.0);
    }

    // One notch moves by roughly the per-notch distance (upward = toward the
    // top = contentY decreases).
    void oneNotchMovesFullNotchDistance()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        // Wheel up: angleDelta +120 → contentY decreases by one notch.
        const double up = c.wheelTargetY(+kNotch, start, kMinY, kMaxY, kViewport);
        QVERIFY(qFuzzyCompare(up, start - per));
    }

    // Several quick same-direction notches coalesce into one extended target,
    // not independent per-event jumps.
    void repeatedNotchesCoalesce()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        // Three notches upward while the animation is still "in flight"
        // (endMotion() not called between them).
        c.wheelTargetY(+kNotch, start, kMinY, kMaxY, kViewport);
        c.wheelTargetY(+kNotch, start - per, kMinY, kMaxY, kViewport);
        const double target = c.wheelTargetY(+kNotch, start - 2 * per,
                                             kMinY, kMaxY, kViewport);
        // Target extended by three notches from the original position.
        QVERIFY(qFuzzyCompare(target, start - 3 * per));
        QVERIFY(c.motionActive());
    }

    // Partial / high-resolution angle deltas contribute proportionally and
    // accumulate rather than being dropped.
    void partialAngleDeltasAccumulate()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        // Three 40-unit deltas sum to one full 120 notch.
        c.wheelTargetY(40.0, start, kMinY, kMaxY, kViewport);
        c.wheelTargetY(40.0, start - per / 3.0, kMinY, kMaxY, kViewport);
        const double target = c.wheelTargetY(40.0, start - 2.0 * per / 3.0,
                                             kMinY, kMaxY, kViewport);
        QVERIFY(qFuzzyCompare(target, start - per));
    }

    // A direction reversal redirects from the live position immediately rather
    // than unwinding a queued opposite-direction target.
    void oppositeDirectionRedirects()
    {
        TimelineScrollController c;
        const double per = c.notchDistance(kViewport);
        const double start = 5000.0;
        // Scroll up (target below start).
        const double upTarget = c.wheelTargetY(+kNotch, start, kMinY, kMaxY,
                                               kViewport);
        QVERIFY(qFuzzyCompare(upTarget, start - per));
        // Now reverse: wheel down while the view has moved to, say, 4950.
        const double live = 4950.0;
        const double downTarget = c.wheelTargetY(-kNotch, live, kMinY, kMaxY,
                                                 kViewport);
        // Redirected from the LIVE position, not extended from upTarget.
        QVERIFY(qFuzzyCompare(downTarget, live + per));
        QVERIFY(downTarget > live);
    }

    // Pixel-delta touchpad input is applied directly, NOT multiplied by the
    // notch distance.
    void pixelDeltaIsNotMultipliedLikeNotch()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        // A 50px two-finger movement upward moves ~50px, nowhere near a notch.
        const double target = c.pixelTargetY(50.0, start, kMinY, kMaxY);
        QVERIFY(qFuzzyCompare(target, start - 50.0));
        const double per = c.notchDistance(kViewport);
        QVERIFY(qAbs(start - target) < 0.5 * per);   // clearly sub-notch.
    }

    // Pixel input cancels any coalesced wheel motion so the two paths never
    // fight over contentY.
    void pixelDeltaCancelsWheelMotion()
    {
        TimelineScrollController c;
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.pixelTargetY(30.0, 4900.0, kMinY, kMaxY);
        QVERIFY(!c.motionActive());
    }

    // The top bound clamps; no negative / invalid content position.
    void clampsAtTopBound()
    {
        TimelineScrollController c;
        // Near the top, a big upward gesture cannot go below minY.
        const double target = c.wheelTargetY(+10.0 * kNotch, 50.0,
                                             kMinY, kMaxY, kViewport);
        QCOMPARE(target, kMinY);
        const double px = c.pixelTargetY(9999.0, 50.0, kMinY, kMaxY);
        QCOMPARE(px, kMinY);
    }

    // The bottom bound clamps.
    void clampsAtBottomBound()
    {
        TimelineScrollController c;
        const double target = c.wheelTargetY(-10.0 * kNotch, kMaxY - 50.0,
                                             kMinY, kMaxY, kViewport);
        QCOMPARE(target, kMaxY);
    }

    // Content shorter than the viewport (maxY < minY) pins to the top and
    // never yields an invalid position.
    void shortContentPinsToTop()
    {
        TimelineScrollController c;
        const double up = c.wheelTargetY(+kNotch, 0.0, 0.0, /*maxY*/ -200.0,
                                         kViewport);
        QCOMPARE(up, 0.0);
        const double down = c.wheelTargetY(-kNotch, 0.0, 0.0, -200.0, kViewport);
        QCOMPARE(down, 0.0);
    }

    // cancel()/endMotion() clear the coalescing state so the next notch starts
    // from the live position.
    void cancelResetsCoalescing()
    {
        TimelineScrollController c;
        const double per = c.notchDistance(kViewport);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.cancel();
        QVERIFY(!c.motionActive());
        // After cancel, a new notch bases off the supplied live position.
        const double target = c.wheelTargetY(+kNotch, 4000.0, kMinY, kMaxY,
                                             kViewport);
        QVERIFY(qFuzzyCompare(target, 4000.0 - per));
    }

    void endMotionEmitsMotionActiveChange()
    {
        TimelineScrollController c;
        QSignalSpy spy(&c, &TimelineScrollController::motionActiveChanged);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QCOMPARE(spy.count(), 1);        // false → true
        c.endMotion();
        QCOMPARE(spy.count(), 2);        // true → false
    }

    // An out-of-range persisted value falls back to Fast rather than an
    // undefined speed.
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

    // The selected speed actually changes the discrete-notch distance.
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
        // Very fast moves the target farther up (smaller contentY) than
        // Standard for the same single notch.
        QVERIFY(vfTarget < stdTarget);
    }

    // The speed setting must NOT rescale pixel-delta touchpad input.
    void speedDoesNotRescalePixelDelta()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::Standard);
        const double stdPx = c.pixelTargetY(50.0, 5000.0, kMinY, kMaxY);
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double vfPx = c.pixelTargetY(50.0, 5000.0, kMinY, kMaxY);
        QVERIFY(qFuzzyCompare(stdPx, vfPx));       // identical: no speed factor
        QVERIFY(qFuzzyCompare(stdPx, 5000.0 - 50.0));
    }

    // ── v0.6.0: wheel motion engine (continuous smoothness) ─────────────
    // The 0.5.19 chunkiness inside one tall wrapped delegate was caused by
    // restarting a fixed-duration OutCubic animation per notch: slow notch
    // cadences produced stop-start bursts (OutCubic ends at zero velocity),
    // fast cadences re-ran the whole remaining distance in a fresh 140 ms.
    // The engine below integrates position toward the coalesced target with
    // continuous velocity; these tests drive advanceMotion() deterministically
    // (16 ms frames) — the same code the frame ticker runs.

    // One notch produces MANY monotonic intermediate positions, not one step
    // per notch — the property a delegate taller than the viewport exposes.
    void motionProgressesThroughIntermediatePositions()
    {
        TimelineScrollController c;
        QSignalSpy frames(&c, &TimelineScrollController::wheelPositionChanged);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);   // downward
        const double target = c.targetYForTest();
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(frames.count() >= 5);
        double prev = 5000.0;
        for (int i = 0; i < frames.count(); ++i) {
            const double y = frames.at(i).at(0).toDouble();
            QVERIFY2(y > prev - 0.001, "position must advance monotonically");
            // No single frame may cover the whole notch in one visible jump.
            QVERIFY2(y - prev < 0.6 * (target - 5000.0),
                     "one frame covered most of the notch — chunky");
            prev = y;
        }
        QVERIFY(qFuzzyCompare(prev, target));
    }

    // Same-direction notches arriving at a realistic cadence (150 ms) keep
    // the motion alive continuously — never a full stop between notches, and
    // the frame after a new notch moves at least as fast as the frame before
    // it (velocity is preserved or raised, never reset).
    void repeatedNotchesPreserveContinuousVelocity()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        double before = 0.0;
        // ~150 ms of frames: motion must still be active when notch 2 lands.
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

    // Reversing direction redirects on the very next frame.
    void reversalRedirectsOnNextFrame()
    {
        TimelineScrollController c;
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);   // down
        c.advanceMotion(16.0);
        c.advanceMotion(16.0);
        const double mid = c.positionYForTest();
        c.wheelNotch(+kNotch, mid, kMinY, kMaxY, kViewport);      // reverse: up
        c.advanceMotion(16.0);
        QVERIFY2(c.positionYForTest() < mid,
                 "reversal did not redirect immediately");
    }

    // Motion settles in bounded time with no asymptotic tail, emits exactly
    // one settle signal, and leaves no ticker running afterwards.
    void motionSettlesWithinBoundedTimeAndStopsCleanly()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        QSignalSpy settled(&c, &TimelineScrollController::wheelMotionSettled);
        // A large coalesced goal: five rapid Very fast notches.
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

    // A stalled frame (long dt) may not integrate one giant visible jump.
    void stalledFrameDoesNotJump()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        const double y0 = c.positionYForTest();
        c.advanceMotion(400.0);   // e.g. window drag stalled the loop
        const double moved = c.positionYForTest() - y0;
        const double total = c.targetYForTest() - 5000.0;
        QVERIFY2(moved < 0.6 * total, "stalled frame jumped most of the way");
    }

    // QML re-clamps emitted positions against live geometry; when it reports
    // a bound was hit, the engine adopts the clamped position and settles
    // instead of pushing into the bound.
    void boundReportSettlesMotion()
    {
        TimelineScrollController c;
        QSignalSpy settled(&c, &TimelineScrollController::wheelMotionSettled);
        c.wheelNotch(+kNotch, 100.0, kMinY, kMaxY, kViewport);   // toward top
        c.advanceMotion(16.0);
        c.notifyBoundReached(kMinY);
        QVERIFY(!c.motionActive());
        QVERIFY(!c.tickerRunningForTest());
        QCOMPARE(settled.count(), 1);
        QCOMPARE(c.positionYForTest(), kMinY);
    }

    // animateTo (keyboard paging) uses the same engine: motion engages
    // synchronously, progresses through intermediate frames, and settles.
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

    // cancel() (room switch, Jump to latest, reply navigation, restore) stops
    // the engine immediately: no further frames, no settle signal — the
    // programmatic caller owns contentY from here.
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
        QVERIFY(!c.advanceMotion(16.0));          // engine refuses to move
        QCOMPARE(frames.count(), framesBefore);
        QCOMPARE(settled.count(), 0);
    }

    // The pixel-delta touchpad path stays direct and precise, and stops any
    // in-flight engine motion (the platform owns momentum there).
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

    // Changing the speed mid-motion is safe and takes effect on the next notch.
    void speedChangeDuringMotionIsSafe()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::Standard);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.setWheelSpeed(TimelineScrollController::VeryFast);   // no crash
        const double perVf = c.notchDistance(kViewport);
        // Next same-direction notch extends using the NEW distance.
        const double before = c.targetYForTest();
        const double after = c.wheelTargetY(+kNotch, 4000.0, kMinY, kMaxY,
                                            kViewport);
        QVERIFY(qFuzzyCompare(after, before - perVf));
    }
};

QTEST_MAIN(TimelineScrollControllerTest)
#include "TimelineScrollControllerTest.moc"
