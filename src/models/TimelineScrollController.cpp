#include "models/TimelineScrollController.h"

namespace {
// Per-notch distance is a fraction of the viewport, clamped to an absolute
// pixel range. Viewport-relative keeps the feel consistent across window
// sizes (never hardcoded to one display); the clamps keep tiny viewports
// moving usefully and very tall ones from lurching a whole screen at once.
//
// Standard is a modest, conventional distance (roughly the 0.5.18 feel);
// Fast — the 0.5.19 default — is clearly farther; VeryFast suits
// low-resolution wheels that emit few notches per turn.
struct SpeedProfile {
    double fraction;
    double minPixels;
    double maxPixels;
};

constexpr SpeedProfile kStandard{0.12, 60.0, 260.0};
constexpr SpeedProfile kFast{0.34, 130.0, 680.0};
constexpr SpeedProfile kVeryFast{0.55, 220.0, 1000.0};

// A sane viewport height to fall back on if QML ever passes a non-positive
// one (e.g. before the pane has a size).
constexpr double kFallbackViewport = 600.0;

const SpeedProfile &profileFor(int speed)
{
    switch (speed) {
    case TimelineScrollController::Standard: return kStandard;
    case TimelineScrollController::VeryFast: return kVeryFast;
    case TimelineScrollController::Fast:
    default:                                 return kFast;
    }
}
}

TimelineScrollController::TimelineScrollController(QObject *parent)
    : QObject(parent)
{
}

void TimelineScrollController::setWheelSpeed(WheelSpeed speed)
{
    if (speed < Standard || speed > VeryFast)
        speed = Fast;
    if (m_wheelSpeed == speed)
        return;
    m_wheelSpeed = speed;
    Q_EMIT wheelSpeedChanged();
}

void TimelineScrollController::setWheelSpeedValue(int value)
{
    if (value < kMinSpeed || value > kMaxSpeed)
        setWheelSpeed(Fast);
    else
        setWheelSpeed(static_cast<WheelSpeed>(value));
}

double TimelineScrollController::notchDistanceForSpeed(int speed,
                                                       double viewportHeight) const
{
    if (!(viewportHeight > 0.0))
        viewportHeight = kFallbackViewport;
    const SpeedProfile &p = profileFor(speed);
    double d = viewportHeight * p.fraction;
    if (d < p.minPixels)
        d = p.minPixels;
    if (d > p.maxPixels)
        d = p.maxPixels;
    return d;
}

double TimelineScrollController::notchDistance(double viewportHeight) const
{
    return notchDistanceForSpeed(m_wheelSpeed, viewportHeight);
}

double TimelineScrollController::clampY(double y, double lo, double hi)
{
    if (hi < lo)
        hi = lo;                 // content shorter than the viewport: pinned.
    if (y < lo)
        return lo;
    if (y > hi)
        return hi;
    return y;
}

void TimelineScrollController::setMotionActive(bool active)
{
    if (m_motionActive == active)
        return;
    m_motionActive = active;
    Q_EMIT motionActiveChanged();
}

double TimelineScrollController::wheelTargetY(double angleDeltaY, double contentY,
                                              double minContentY,
                                              double maxContentY,
                                              double viewportHeight)
{
    const double per = notchDistance(viewportHeight);
    // angleDelta.y > 0 == wheel rotated up == scroll toward the top, so
    // contentY must decrease. Proportional conversion means a partial or
    // high-resolution delta (e.g. 40 or 15) contributes its exact fraction of
    // a notch instead of being dropped or rounded up to a whole notch.
    const double deltaPixels = -(angleDeltaY / 120.0) * per;
    const int dir = deltaPixels > 0.0 ? 1 : (deltaPixels < 0.0 ? -1 : 0);
    if (dir == 0)
        return clampY(m_motionActive ? m_targetY : contentY,
                      minContentY, maxContentY);

    // Coalesce only when a motion is in flight AND this event continues in the
    // same direction: extend the existing goal so several quick notches build
    // one smooth movement. A reversal (or a fresh gesture) redirects from the
    // live position so the change of direction is felt immediately.
    const double base = (m_motionActive && dir == m_direction) ? m_targetY
                                                               : contentY;
    const double target = clampY(base + deltaPixels, minContentY, maxContentY);
    m_targetY = target;
    m_direction = dir;
    setMotionActive(true);
    return target;
}

double TimelineScrollController::pixelTargetY(double pixelDeltaY, double contentY,
                                              double minContentY,
                                              double maxContentY)
{
    // The platform's own momentum arrives as a stream of pixel-delta events;
    // applying them directly preserves that momentum without a competing
    // animation. Drop any coalesced wheel goal so the two paths never fight.
    cancel();
    // pixelDelta.y > 0 == scroll toward the top == contentY decreases.
    return clampY(contentY - pixelDeltaY * m_pixelFactor,
                  minContentY, maxContentY);
}

void TimelineScrollController::endMotion()
{
    m_direction = 0;
    setMotionActive(false);
}

void TimelineScrollController::cancel()
{
    m_direction = 0;
    setMotionActive(false);
}
