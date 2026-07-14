#pragma once

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

// v0.5.19: device-aware timeline wheel-scroll policy.
//
// Lightning's timeline is a plain Qt Quick ListView. In 0.5.18 it relied
// entirely on Flickable's built-in wheel handling, which maps one physical
// mouse-wheel notch to only a few lines — the confirmed cause of the
// "must rotate the wheel many times" complaint. This controller owns the
// SCROLL MATH so it can be unit-tested deterministically: the offscreen QPA
// platform used by the QML tests never incubates ListView delegates, so
// geometry-dependent behaviour cannot be exercised through the real view.
// QML supplies live geometry and runs the animation; this class decides how
// far to move and keeps the three movement sources cleanly separated:
//
//   * discrete angle-delta mouse wheel -> wheelTargetY(): a bounded per-notch
//     distance derived from the viewport and the selected speed, with rapid
//     notches coalesced into one reusable animation and partial/high-res
//     angle deltas accumulated rather than dropped;
//   * high-resolution pixel-delta touchpad / precision wheel -> pixelTargetY():
//     applied directly (mild bounded scaling only, never the notch
//     multiplier) so fine movement and native momentum are preserved;
//   * programmatic navigation (Jump to latest, reply target, anchor restore,
//     room switch) -> NOT routed here; QML cancels any active wheel motion via
//     cancel() so restoration is never mistaken for physical wheel input.
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
    // True while a coalesced discrete-wheel animation owns the viewport, so
    // QML can treat programmatic contentY changes as user-intent for
    // follow-latest / pagination without waiting for Flickable.moving.
    Q_PROPERTY(bool motionActive READ motionActive NOTIFY motionActiveChanged)

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

    // Pixels one full wheel notch (angleDelta.y == 120) scrolls at the active
    // speed, given the current viewport height. Bounded so tiny viewports
    // still move usefully and huge ones never jump uncontrollably.
    Q_INVOKABLE double notchDistance(double viewportHeight) const;
    Q_INVOKABLE double notchDistanceForSpeed(int speed, double viewportHeight) const;

    // Discrete angle-delta wheel. Returns the absolute contentY the view
    // should animate to, clamped to [minContentY, maxContentY]. angleDeltaY is
    // Qt's WheelEvent.angleDelta.y (>0 = wheel up = scroll toward older/top,
    // so contentY decreases). Same-direction input extends the in-flight
    // target (coalescing); a direction reversal redirects from the live
    // position. Partial/high-resolution deltas contribute proportionally.
    Q_INVOKABLE double wheelTargetY(double angleDeltaY, double contentY,
                                    double minContentY, double maxContentY,
                                    double viewportHeight);

    // High-resolution pixel-delta (touchpad / precision wheel). Returns the
    // contentY to jump to directly, with only mild bounded scaling — never the
    // notch multiplier. Cancels any coalesced wheel target: the platform owns
    // momentum on this path, so a second animation must not fight it.
    Q_INVOKABLE double pixelTargetY(double pixelDeltaY, double contentY,
                                    double minContentY, double maxContentY);

    // QML reports the coalescing animation ran to completion (or was stopped),
    // so the next notch starts from the live position, not a stale target.
    Q_INVOKABLE void endMotion();
    // Hard cancel: room/account change, programmatic navigation, destruction.
    Q_INVOKABLE void cancel();

    // Test hooks.
    void setPixelFactorForTest(double factor) { m_pixelFactor = factor; }
    double targetYForTest() const { return m_targetY; }

Q_SIGNALS:
    void wheelSpeedChanged();
    void motionActiveChanged();

private:
    static double clampY(double y, double lo, double hi);
    void setMotionActive(bool active);

    WheelSpeed m_wheelSpeed = Fast;

    // Coalesced discrete-wheel goal. Valid only while m_motionActive.
    double m_targetY = 0.0;
    int m_direction = 0;         // -1 = toward top, +1 = toward bottom, 0 = none
    bool m_motionActive = false;

    // Touchpad pixel scaling. 1.0 == native; kept mild and tunable.
    double m_pixelFactor = 1.0;
};
