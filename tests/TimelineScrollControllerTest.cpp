// The timeline wheel-scroll policy in TimelineScrollController, tested in
// isolation because the offscreen QPA does not incubate list delegates:
// per-notch distance and ordering, coalescing, partial-delta accumulation,
// direction reversal, pixel-delta vs angle-delta, bound clamping and motion
// cancellation.

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
    // The default is Fast, and one notch moves farther than Standard.
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
        // A Fast notch is a meaningful fraction of the viewport (several
        // lines).
        QVERIFY(fast >= 0.25 * kViewport);
    }

    // Per-notch distance scales with the viewport within absolute bounds.
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

    // One notch moves by the per-notch distance (upward = contentY
    // decreases).
    void oneNotchMovesFullNotchDistance()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        // Wheel up: angleDelta +120 decreases contentY by one notch.
        const double up = c.wheelTargetY(+kNotch, start, kMinY, kMaxY, kViewport);
        QVERIFY(qFuzzyCompare(up, start - per));
    }

    // Quick same-direction notches coalesce into one extended target.
    void repeatedNotchesCoalesce()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        const double per = c.notchDistance(kViewport);
        // Three upward notches while the motion is in flight (no endMotion()
        // between them).
        c.wheelTargetY(+kNotch, start, kMinY, kMaxY, kViewport);
        c.wheelTargetY(+kNotch, start - per, kMinY, kMaxY, kViewport);
        const double target = c.wheelTargetY(+kNotch, start - 2 * per,
                                             kMinY, kMaxY, kViewport);
        // The target is three notches from the original position.
        QVERIFY(qFuzzyCompare(target, start - 3 * per));
        QVERIFY(c.motionActive());
    }

    // Partial (high-resolution) angle deltas accumulate proportionally.
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

    // A direction reversal redirects from the live position rather than
    // unwinding a queued target.
    void oppositeDirectionRedirects()
    {
        TimelineScrollController c;
        const double per = c.notchDistance(kViewport);
        const double start = 5000.0;
        // Scroll up (target below start).
        const double upTarget = c.wheelTargetY(+kNotch, start, kMinY, kMaxY,
                                               kViewport);
        QVERIFY(qFuzzyCompare(upTarget, start - per));
        // Reverse while the view is at, say, 4950.
        const double live = 4950.0;
        const double downTarget = c.wheelTargetY(-kNotch, live, kMinY, kMaxY,
                                                 kViewport);
        // Redirected from the live position, not extended from upTarget.
        QVERIFY(qFuzzyCompare(downTarget, live + per));
        QVERIFY(downTarget > live);
    }

    // Pixel-delta (touchpad) input applies directly, not multiplied like a
    // notch.
    void pixelDeltaIsNotMultipliedLikeNotch()
    {
        TimelineScrollController c;
        const double start = 5000.0;
        // A 50 px upward movement moves ~50 px.
        const double target = c.pixelTargetY(50.0, start, kMinY, kMaxY);
        QVERIFY(qFuzzyCompare(target, start - 50.0));
        const double per = c.notchDistance(kViewport);
        QVERIFY(qAbs(start - target) < 0.5 * per);   // clearly sub-notch.
    }

    // Pixel input cancels coalesced wheel motion, so the two paths never
    // fight over contentY.
    void pixelDeltaCancelsWheelMotion()
    {
        TimelineScrollController c;
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.pixelTargetY(30.0, 4900.0, kMinY, kMaxY);
        QVERIFY(!c.motionActive());
    }

    // The top bound clamps.
    void clampsAtTopBound()
    {
        TimelineScrollController c;
        // Near the top, a large upward gesture cannot pass minY.
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

    // Content shorter than the viewport (maxY < minY) pins to the top.
    void shortContentPinsToTop()
    {
        TimelineScrollController c;
        const double up = c.wheelTargetY(+kNotch, 0.0, 0.0, /*maxY*/ -200.0,
                                         kViewport);
        QCOMPARE(up, 0.0);
        const double down = c.wheelTargetY(-kNotch, 0.0, 0.0, -200.0, kViewport);
        QCOMPARE(down, 0.0);
    }

    // cancel()/endMotion() clear coalescing, so the next notch starts from
    // the live position.
    void cancelResetsCoalescing()
    {
        TimelineScrollController c;
        const double per = c.notchDistance(kViewport);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.cancel();
        QVERIFY(!c.motionActive());
        // After cancel, a new notch bases on the supplied live position.
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

    // The per-gesture scroll diagnostics follow LIGHTNING_SCROLL_TRACE, read
    // once at construction; off by default.
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

    // An out-of-range persisted speed falls back to Fast.
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

    // The speed setting changes the discrete-notch distance.
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
        // Very fast moves farther (smaller contentY) than Standard for one
        // notch.
        QVERIFY(vfTarget < stdTarget);
    }

    // The speed setting does not rescale pixel-delta input.
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

    // Wheel motion engine: position is integrated toward the coalesced target
    // with continuous velocity, driven here by advanceMotion() in 16 ms frames
    // (the code the frame ticker runs).
    //
    // One notch produces many monotonic intermediate positions, not one step.
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
            // No single frame covers most of the notch.
            QVERIFY2(y - prev < 0.6 * (target - 5000.0),
                     "one frame covered most of the notch — chunky");
            prev = y;
        }
        QVERIFY(qFuzzyCompare(prev, target));
    }

    // Same-direction notches at a realistic cadence (150 ms) keep the motion
    // alive, and velocity is preserved or raised, never reset.
    void repeatedNotchesPreserveContinuousVelocity()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        c.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        double before = 0.0;
        // ~150 ms of frames: motion is still active when notch 2 lands.
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

    // Motion settles in bounded time, emits one settle signal, and leaves no
    // ticker running.
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

    // A stalled frame (long dt) does not integrate one giant jump.
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

    // When QML reports a bound was hit, the engine adopts the clamped position
    // and settles.
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

    // translateActiveMotion() shifts an in-flight glide (position and target
    // together) by a prepend's height, so the remaining distance is unchanged
    // and the glide finishes at a destination that moved with the content,
    // rather than being cancelled mid-flight.
    void translateActiveMotionPreservesRemainingDistance()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        // A real coalesced glide with remaining distance, as when the reader
        // spins the wheel during a near-top request.
        for (int i = 0; i < 4; ++i)
            c.wheelNotch(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        // Advance partway, as when the batch lands mid-flight.
        for (int i = 0; i < 3; ++i)
            c.advanceMotion(16.0);
        QVERIFY(c.motionActive());
        const double remainingBefore = c.targetYForTest() - c.positionYForTest();

        constexpr double shift = 240.0;   // height of the prepended content
        const double targetBefore = c.targetYForTest();
        c.translateActiveMotion(shift);

        // Still active, same remaining distance, relocated by the shift.
        QVERIFY(c.motionActive());
        QVERIFY(qFuzzyCompare(c.targetYForTest() - c.positionYForTest(),
                              remainingBefore));
        QVERIFY(qFuzzyCompare(c.targetYForTest(), targetBefore + shift));

        // It lands exactly on the shifted target.
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(qFuzzyCompare(c.positionYForTest(), c.targetYForTest()));
        QVERIFY(qFuzzyCompare(c.positionYForTest(), targetBefore + shift));
    }

    // With no motion in flight there is nothing to translate: a true no-op
    // (the QML caller then writes contentY itself).
    void translateActiveMotionIsNoOpWhenNotActive()
    {
        TimelineScrollController c;
        QVERIFY(!c.motionActive());
        c.translateActiveMotion(500.0);
        QVERIFY(!c.motionActive());
        QCOMPARE(c.positionYForTest(), 0.0);
        QCOMPARE(c.targetYForTest(), 0.0);
    }

    // A zero shift leaves an in-flight glide untouched.
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

    // animateTo (keyboard paging) uses the same engine: engages synchronously,
    // progresses through frames, and settles.
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

    // cancel() stops the engine immediately with no further frames and no
    // settle signal; the programmatic caller owns contentY.
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

    // The pixel path stays direct and stops any in-flight engine motion (the
    // platform owns momentum there).
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

    // Changing speed mid-motion is safe and applies from the next notch.
    void speedChangeDuringMotionIsSafe()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::Standard);
        c.wheelTargetY(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        c.setWheelSpeed(TimelineScrollController::VeryFast);   // no crash
        const double perVf = c.notchDistance(kViewport);
        // The next same-direction notch extends using the new distance.
        const double before = c.targetYForTest();
        const double after = c.wheelTargetY(+kNotch, 4000.0, kMinY, kMaxY,
                                            kViewport);
        QVERIFY(qFuzzyCompare(after, before - perVf));
    }

    // Geometry, finiteness and controller independence.
    //
    // Even at Very fast over a tall viewport (the largest bounded notch), a
    // notch produces many monotonic frames, none covering most of it.
    void tallDelegateStillProducesSmoothFrames()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double tallViewport = 1600.0;   // a maximised window
        QSignalSpy frames(&c, &TimelineScrollController::wheelPositionChanged);
        c.wheelNotch(-kNotch, 5000.0, kMinY, /*maxY*/ 100000.0, tallViewport);
        const double target = c.targetYForTest();
        const double total = target - 5000.0;
        QVERIFY(total > tallViewport * 0.3);   // genuinely a big move
        double prev = 5000.0;
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(frames.count() >= 6);
        for (int i = 0; i < frames.count(); ++i) {
            const double y = frames.at(i).at(0).toDouble();
            QVERIFY(y > prev - 0.001);                 // monotonic
            QVERIFY2(y - prev < 0.6 * total, "one frame covered most of it");
            prev = y;
        }
        QVERIFY(qFuzzyCompare(prev, target));
    }

    // Notches at a slow cadence (each settling before the next) are each
    // smooth: several frames and no single-frame lurch.
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

    // Content shrinking mid-motion: QML reports the new bound and the engine
    // settles exactly at the clamped position.
    void contentShrinkDuringMotionDoesNotOverscroll()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        c.wheelNotch(-kNotch, 5000.0, kMinY, /*maxY*/ 10000.0, kViewport);
        c.advanceMotion(16.0);
        const double target = c.targetYForTest();
        // The content shrank so the new bottom is above the in-flight target.
        const double newMax = target - 40.0;
        c.notifyBoundReached(newMax);
        QVERIFY(!c.motionActive());
        QVERIFY(!c.tickerRunningForTest());
        QCOMPARE(c.positionYForTest(), newMax);
        // No residual motion pushes past the shrunk bound.
        QVERIFY(!c.advanceMotion(16.0));
        QVERIFY(c.positionYForTest() <= newMax);
    }

    // The main timeline and thread panel controllers are independent; driving
    // one never moves the other.
    void mainAndThreadControllersAreIndependent()
    {
        TimelineScrollController main;
        TimelineScrollController thread;

        main.wheelNotch(-kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(main.motionActive());
        QVERIFY(!thread.motionActive());       // thread untouched
        QVERIFY(!thread.tickerRunningForTest());

        // Advancing main leaves the thread controller at its defaults.
        main.advanceMotion(16.0);
        QCOMPARE(thread.positionYForTest(), 0.0);

        // Drive the thread; main keeps its own target.
        const double mainTarget = main.targetYForTest();
        thread.setWheelSpeed(TimelineScrollController::VeryFast);
        thread.wheelNotch(+kNotch, 800.0, kMinY, kMaxY, kViewport);
        QVERIFY(thread.motionActive());
        QCOMPARE(main.targetYForTest(), mainTarget);   // main goal unchanged

        // Cancelling one never settles the other.
        thread.cancel();
        QVERIFY(!thread.motionActive());
        QVERIFY(main.motionActive());
    }

    // Across a stress sequence (extreme deltas, huge room, reversals, a stall,
    // bound clamps) position and target stay finite.
    void positionsAndTargetsStayFinite()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        const double bigMax = 1.0e9;
        c.wheelNotch(-12.0 * kNotch, 5000.0, kMinY, bigMax, 2000.0);
        for (int i = 0; i < 60; ++i) {
            if (i == 15)   // reverse mid-flight
                c.wheelNotch(+8.0 * kNotch, c.positionYForTest(), kMinY,
                             bigMax, 2000.0);
            if (i == 30)
                c.advanceMotion(1000.0);   // a long stall
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

    // Two growth corrections in one gesture (two images above the anchor
    // resolving) compose additively via translateActiveMotion(): remaining
    // distance is preserved across both and the glide settles on the fully
    // composed target.
    void translateActiveMotionComposesAcrossRepeatedGrowthCorrections()
    {
        TimelineScrollController c;
        c.setWheelSpeed(TimelineScrollController::VeryFast);
        // A coalesced upward glide with remaining distance, as media above
        // resolves.
        for (int i = 0; i < 4; ++i)
            c.wheelNotch(+kNotch, 5000.0, kMinY, kMaxY, kViewport);
        QVERIFY(c.motionActive());
        for (int i = 0; i < 2; ++i)
            c.advanceMotion(16.0);
        const double remainingBefore =
            c.targetYForTest() - c.positionYForTest();
        const double targetBefore = c.targetYForTest();

        // The first image row resolves: its estimated height (~50 px) becomes
        // its real height (~320 px), ~270 px of growth above the anchor.
        constexpr double firstGrowth = 270.0;
        c.translateActiveMotion(firstGrowth);
        QVERIFY(c.motionActive());
        QVERIFY(qFuzzyCompare(c.targetYForTest() - c.positionYForTest(),
                              remainingBefore));
        QVERIFY(qFuzzyCompare(c.targetYForTest(),
                              targetBefore + firstGrowth));

        // A second image row above it resolves in the same gesture.
        c.advanceMotion(16.0);
        const double remainingMid = c.targetYForTest() - c.positionYForTest();
        constexpr double secondGrowth = 300.0;
        c.translateActiveMotion(secondGrowth);
        QVERIFY(c.motionActive());
        QVERIFY(qFuzzyCompare(c.targetYForTest() - c.positionYForTest(),
                              remainingMid));
        QVERIFY(qFuzzyCompare(c.targetYForTest(),
                              targetBefore + firstGrowth + secondGrowth));

        // It settles on the fully composed target: neither correction lost nor
        // doubled.
        while (c.motionActive())
            c.advanceMotion(16.0);
        QVERIFY(qFuzzyCompare(c.positionYForTest(), c.targetYForTest()));
        QVERIFY(qFuzzyCompare(c.positionYForTest(),
                              targetBefore + firstGrowth + secondGrowth));
    }

    // Growth compensation composes the same way during a downward glide.
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
};

QTEST_MAIN(TimelineScrollControllerTest)
#include "TimelineScrollControllerTest.moc"
