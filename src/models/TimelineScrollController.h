#pragma once

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

class QAbstractAnimation;

// v0.5.19: device-aware timeline wheel-scroll policy.
// v0.6.0:  wheel-only motion engine for continuously smooth movement.
//
// Lightning's timeline is a plain Qt Quick ListView. In 0.5.18 it relied
// entirely on Flickable's built-in wheel handling, which maps one physical
// mouse-wheel notch to only a few lines — the confirmed cause of the
// "must rotate the wheel many times" complaint. This controller owns the
// SCROLL MATH so it can be unit-tested deterministically: the offscreen QPA
// platform used by the QML tests never incubates ListView delegates, so
// geometry-dependent behaviour cannot be exercised through the real view.
//
// 0.5.19 computed a coalesced target here but let QML animate contentY with a
// single fixed-duration OutCubic NumberAnimation that was STOPPED AND
// RESTARTED on every notch. That restart pattern was the confirmed cause of
// "chunky" movement inside one tall wrapped message: OutCubic ends at zero
// velocity, so slow notch cadences produced stop-start bursts, while fast
// cadences re-ran the whole remaining distance in a fresh fixed 140 ms —
// a sawtooth velocity profile that item boundaries normally mask but a
// delegate taller than the viewport exposes.
//
// 0.6.0 therefore moves the MOTION itself into this class. A wheel-only
// ticker (a QAbstractAnimation driven by Qt's animation driver, running only
// while motion is active — never a permanent frame timer) integrates the
// position toward the coalesced target each frame:
//
//   step = remaining * (1 - exp(-dt/tau))        // exponential approach
//   |step| >= minSettleSpeed * dt                // bounded tail, quick stop
//
// Velocity is therefore continuous within a gesture, same-direction notches
// raise it smoothly (the remaining distance grows), a reversal redirects
// immediately (remaining flips sign), and motion always settles in bounded
// time with no momentum tail. QML applies emitted positions to contentY and
// re-clamps against live geometry; programmatic navigation (Jump to latest,
// reply target, anchor restore, room switch, Home/End) bypasses the engine
// entirely via cancel().
//
// The three movement sources remain cleanly separated:
//
//   * discrete angle-delta mouse wheel -> wheelNotch(): a bounded per-notch
//     distance derived from the viewport and the selected speed, coalesced
//     into one continuous motion; partial/high-res deltas accumulate;
//   * high-resolution pixel-delta touchpad / precision wheel -> pixelTargetY():
//     applied directly (mild bounded scaling only, never the notch
//     multiplier) so fine movement and native momentum are preserved;
//   * programmatic navigation -> NOT routed here; QML cancels any active
//     wheel motion via cancel() first.
//
// Never logs; carries no message content, room ids, or URLs.
class TimelineScrollController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Instantiated in C++ and exposed to QML only as the "app.timelineScroll"
    // context-property instance; this registration exists so QML can name the
    // WheelSpeed enum as TimelineScrollController.Fast etc.
    QML_UNCREATABLE("TimelineScrollController is exposed via app.timelineScroll")
    Q_PROPERTY(WheelSpeed wheelSpeed READ wheelSpeed WRITE setWheelSpeed
                   NOTIFY wheelSpeedChanged)
    // True while the wheel motion engine owns the viewport, so QML can treat
    // programmatic contentY changes as user-intent for follow-latest /
    // pagination without waiting for Flickable.moving.
    Q_PROPERTY(bool motionActive READ motionActive NOTIFY motionActiveChanged)
    // Bounded per-gesture scroll diagnostics, off unless the environment
    // variable LIGHTNING_SCROLL_TRACE is set. When on, TimelinePane emits ONE
    // summarized line per wheel/touchpad gesture at settle (never per event):
    // event count, device mix, net contentY movement, content-height churn,
    // and — the load-bearing number — how many times a deferred anchor
    // correction actually wrote the position while the gesture owned it (must
    // be 0). Read once at construction; never logs message content or ids.
    Q_PROPERTY(bool scrollTraceEnabled READ scrollTraceEnabled CONSTANT)

public:
    // Persisted as a stable integer (see SettingsManager). Order matters:
    // Standard < Fast < VeryFast by per-notch distance.
    enum WheelSpeed { Standard = 0, Fast = 1, VeryFast = 2 };
    Q_ENUM(WheelSpeed)

    static constexpr int kMinSpeed = Standard;
    static constexpr int kMaxSpeed = VeryFast;

    explicit TimelineScrollController(QObject *parent = nullptr);

    WheelSpeed wheelSpeed() const { return m_wheelSpeed; }
    void setWheelSpeed(WheelSpeed speed);
    // Convenience for the settings bridge, which stores a plain int. An
    // out-of-range value falls back to Fast rather than an undefined speed.
    Q_INVOKABLE void setWheelSpeedValue(int value);

    bool motionActive() const { return m_motionActive; }
    bool scrollTraceEnabled() const { return m_scrollTraceEnabled; }

    // Pixels one full wheel notch (angleDelta.y == 120) scrolls at the active
    // speed, given the current viewport height. Bounded so tiny viewports
    // still move usefully and huge ones never jump uncontrollably.
    Q_INVOKABLE double notchDistance(double viewportHeight) const;
    Q_INVOKABLE double notchDistanceForSpeed(int speed, double viewportHeight) const;

    // Discrete angle-delta wheel input: updates the coalesced target and
    // (re)engages the motion engine. angleDeltaY is Qt's WheelEvent
    // angleDelta.y (>0 = wheel up = scroll toward older/top, so contentY
    // decreases). Same-direction input extends the in-flight target; a
    // reversal redirects from the live position. Partial/high-resolution
    // deltas contribute proportionally. contentY seeds the engine position
    // only when no motion is in flight — mid-motion the engine's own
    // integrated position is authoritative.
    Q_INVOKABLE void wheelNotch(double angleDeltaY, double contentY,
                                double minContentY, double maxContentY,
                                double viewportHeight);

    // Smooth motion to an absolute target (keyboard paging: Page Up/Down,
    // Space). Reuses the same engine so repeated presses never queue and
    // coalesce exactly like wheel notches.
    Q_INVOKABLE void animateTo(double targetY, double contentY,
                               double minContentY, double maxContentY);

    // Pure target computation for the coalescing policy (also drives
    // wheelNotch). Returns the absolute contentY goal, clamped to
    // [minContentY, maxContentY]. Kept invokable-free of motion so the policy
    // stays independently testable.
    Q_INVOKABLE double wheelTargetY(double angleDeltaY, double contentY,
                                    double minContentY, double maxContentY,
                                    double viewportHeight);

    // High-resolution pixel-delta (touchpad / precision wheel). Returns the
    // contentY to jump to directly, with only mild bounded scaling — never the
    // notch multiplier. Cancels any coalesced wheel target: the platform owns
    // momentum on this path, so a second animation must not fight it.
    Q_INVOKABLE double pixelTargetY(double pixelDeltaY, double contentY,
                                    double minContentY, double maxContentY);

    // QML re-clamped an emitted position against live geometry (content grew
    // or shrank mid-motion) and it hit a bound: adopt the clamped position and
    // settle instead of pushing further into the bound.
    Q_INVOKABLE void notifyBoundReached(double clampedY);

    // Legacy explicit end-of-motion (tests); the engine normally settles
    // itself and emits wheelMotionSettled().
    Q_INVOKABLE void endMotion();
    // Hard cancel: room/account change, programmatic navigation, destruction.
    // Stops the ticker without emitting a final position or settle signal —
    // the caller owns contentY from here.
    Q_INVOKABLE void cancel();

    // Internal engine step; public so the frame ticker and deterministic
    // tests share the exact same integration code. Returns false once motion
    // has settled (the ticker stops itself on that).
    bool advanceMotion(double dtMs);

    // Test hooks.
    void setPixelFactorForTest(double factor) { m_pixelFactor = factor; }
    double targetYForTest() const { return m_targetY; }
    double positionYForTest() const { return m_positionY; }
    bool tickerRunningForTest() const;

Q_SIGNALS:
    void wheelSpeedChanged();
    void motionActiveChanged();
    // One frame of wheel motion: QML applies this to contentY (re-clamping
    // against live geometry).
    void wheelPositionChanged(double contentY);
    // Motion reached its target (or a bound) and stopped. QML recomputes
    // follow-latest / pagination and schedules ONE settled anchor save.
    void wheelMotionSettled();

private:
    static double clampY(double y, double lo, double hi);
    void setMotionActive(bool active);
    void startEngine(double targetY, double contentY,
                     double minContentY, double maxContentY, int direction);
    void startTicker();
    void stopTicker();
    void settle();

    WheelSpeed m_wheelSpeed = Fast;

    // Coalesced wheel goal + engine state. Valid only while m_motionActive.
    double m_targetY = 0.0;
    double m_positionY = 0.0;
    int m_direction = 0;         // -1 = toward top, +1 = toward bottom, 0 = none
    bool m_motionActive = false;

    // Live clamping bounds supplied with the most recent motion request.
    double m_minY = 0.0;
    double m_maxY = 0.0;

    // Touchpad pixel scaling. 1.0 == native; kept mild and tunable.
    double m_pixelFactor = 1.0;

    // Bounded per-gesture scroll diagnostics gate (env LIGHTNING_SCROLL_TRACE),
    // read once at construction.
    bool m_scrollTraceEnabled = false;

    // Frame ticker; parented to this, running only while motion is active.
    QAbstractAnimation *m_ticker = nullptr;
};
