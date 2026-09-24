#include "models/TimelineScrollController.h"

#include <QAbstractAnimation>

#include <algorithm>
#include <cmath>

namespace {
// Per-notch distance is a fraction of the viewport, clamped to an absolute
// pixel range, so the feel is consistent across window sizes. VeryFast suits
// low-resolution wheels that emit few notches per turn.
struct SpeedProfile {
    double fraction;
    double minPixels;
    double maxPixels;
};

constexpr SpeedProfile kStandard{0.12, 60.0, 260.0};
constexpr SpeedProfile kFast{0.34, 130.0, 680.0};
constexpr SpeedProfile kVeryFast{0.55, 220.0, 1000.0};

// Fallback when QML passes a non-positive height (pane not sized yet).
constexpr double kFallbackViewport = 600.0;

// Motion tuning. Each frame covers (1 - exp(-dt/tau)) of the remaining
// distance, so speed tracks what is left and rises again when a notch extends
// the target. The minimum settle speed bounds the exponential tail and the
// snap distance ends it exactly. A dt clamp keeps a stalled frame from
// integrating one giant jump.
constexpr double kTauMs = 90.0;
constexpr double kMinSettleSpeed = 700.0;   // px/s
constexpr double kSnapDistance = 0.5;       // px
constexpr double kMaxTickMs = 50.0;

// Hard ceiling on how far one emitted frame may move contentY, as a fraction
// of the viewport. Not a feel knob: QQuickTableView::viewportMoved() ->
// scheduleRebuildIfFastFlick() (Qt 6.11) rebuilds around a guessed top row
// whenever a single contentY write leaves the previous viewport rect. The
// guess uses one average row height, which for a timeline lands far from the
// row being read, so the reader briefly sees another part of the conversation.
// Large single-frame steps happen after a long layout frame (e.g. a history
// page landing) while notches keep coalescing. Half a viewport keeps the rects
// overlapping with margin; the cap is only reached at the start of big glides.
constexpr double kMaxStepViewportFraction = 0.5;

const SpeedProfile &profileFor(int speed)
{
    switch (speed) {
    case TimelineScrollController::Standard: return kStandard;
    case TimelineScrollController::VeryFast: return kVeryFast;
    case TimelineScrollController::Fast:
    default:                                 return kFast;
    }
}

// Frame source for the motion engine, driven by Qt's animation driver and
// running only while motion is in flight.
class WheelTicker : public QAbstractAnimation
{
public:
    explicit WheelTicker(TimelineScrollController *controller)
        : QAbstractAnimation(controller), m_controller(controller) {}

    int duration() const override { return -1; }   // runs until stopped

protected:
    void updateCurrentTime(int currentTime) override
    {
        const int dt = currentTime - m_lastMs;
        m_lastMs = currentTime;
        if (dt <= 0)
            return;
        if (!m_controller->advanceMotion(dt))
            stop();
    }

    void updateState(State newState, State) override
    {
        if (newState == Running)
            m_lastMs = 0;
    }

private:
    TimelineScrollController *m_controller;
    int m_lastMs = 0;
};
}

TimelineScrollController::TimelineScrollController(QObject *parent)
    : QObject(parent)
    , m_scrollTraceEnabled(qEnvironmentVariableIsSet("LIGHTNING_SCROLL_TRACE"))
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
    // angleDelta.y > 0 means wheel up, so contentY decreases. Partial or
    // high-resolution deltas contribute their exact fraction of a notch.
    const double deltaPixels = -(angleDeltaY / 120.0) * per;
    const int dir = deltaPixels > 0.0 ? 1 : (deltaPixels < 0.0 ? -1 : 0);
    if (dir == 0)
        return clampY(m_motionActive ? m_targetY : contentY,
                      minContentY, maxContentY);

    // Extend the goal only for same-direction input during motion, so quick
    // notches build one movement; a reversal or new gesture redirects from the
    // live position.
    const double base = (m_motionActive && dir == m_direction) ? m_targetY
                                                               : contentY;
    const double target = clampY(base + deltaPixels, minContentY, maxContentY);
    m_targetY = target;
    m_direction = dir;
    setMotionActive(true);
    return target;
}

void TimelineScrollController::wheelNotch(double angleDeltaY, double contentY,
                                          double minContentY, double maxContentY,
                                          double viewportHeight)
{
    // Mid-motion the integrated position is authoritative (QML's contentY may
    // lag a frame); otherwise seed from the view.
    if (!m_motionActive)
        m_positionY = clampY(contentY, minContentY, maxContentY);
    m_minY = minContentY;
    m_maxY = maxContentY;
    setViewportHeight(viewportHeight);
    wheelTargetY(angleDeltaY, m_positionY, minContentY, maxContentY,
                 viewportHeight);
    if (!m_motionActive)
        return;                    // zero-delta event: nothing to do.
    startTicker();                 // idempotent while already running.
}

void TimelineScrollController::setViewportHeight(double viewportHeight)
{
    // Ignore a non-positive height rather than disabling the step cap.
    if (viewportHeight > 0.0)
        m_viewportHeight = viewportHeight;
}

void TimelineScrollController::animateTo(double targetY, double contentY,
                                         double minContentY, double maxContentY,
                                         double viewportHeight)
{
    if (!m_motionActive)
        m_positionY = clampY(contentY, minContentY, maxContentY);
    m_minY = minContentY;
    m_maxY = maxContentY;
    setViewportHeight(viewportHeight);
    m_targetY = clampY(targetY, minContentY, maxContentY);
    m_direction = m_targetY < m_positionY ? -1
                  : (m_targetY > m_positionY ? 1 : m_direction);
    setMotionActive(true);
    startTicker();
}

double TimelineScrollController::pixelTargetY(double pixelDeltaY, double contentY,
                                              double minContentY,
                                              double maxContentY)
{
    // Platform momentum arrives as pixel-delta events; apply them directly and
    // drop any coalesced wheel goal so the paths never fight.
    cancel();
    // pixelDelta.y > 0 == scroll toward the top == contentY decreases.
    return clampY(contentY - pixelDeltaY * m_pixelFactor,
                  minContentY, maxContentY);
}

void TimelineScrollController::notifyBoundReached(double clampedY)
{
    if (!m_motionActive)
        return;
    m_positionY = clampedY;
    m_targetY = clampedY;
    settle();
}

double TimelineScrollController::motionStep(double remaining, double dtMs,
                                            double viewportHeight) const
{
    // advanceMotion's per-frame maths without state: exponential approach,
    // minimum settle speed, and per-frame ceiling.
    if (dtMs <= 0.0)
        return 0.0;
    dtMs = std::min(dtMs, kMaxTickMs);
    const double absRemaining = std::abs(remaining);
    if (absRemaining <= kSnapDistance)
        return remaining;

    double step = remaining * (1.0 - std::exp(-dtMs / kTauMs));
    const double minStep = kMinSettleSpeed * dtMs / 1000.0;
    if (std::abs(step) < minStep)
        step = std::copysign(std::min(minStep, absRemaining), remaining);

    const double maxStep = viewportHeight * kMaxStepViewportFraction;
    if (maxStep > 0.0 && std::abs(step) > maxStep)
        step = std::copysign(maxStep, remaining);
    return step;
}

void TimelineScrollController::translateActiveMotion(double deltaY)
{
    // Nothing in flight: the caller cancels instead. A zero shift is a no-op.
    if (!m_motionActive || deltaY == 0.0)
        return;
    m_positionY += deltaY;
    m_targetY += deltaY;
    // The remaining distance is unchanged, so the next frame continues the same
    // curve toward the moved target. QML re-clamps every emitted frame.
}

bool TimelineScrollController::advanceMotion(double dtMs)
{
    if (!m_motionActive)
        return false;
    if (dtMs <= 0.0)
        return true;
    dtMs = std::min(dtMs, kMaxTickMs);

    const double remaining = m_targetY - m_positionY;
    const double absRemaining = std::abs(remaining);
    if (absRemaining <= kSnapDistance) {
        m_positionY = m_targetY;
        Q_EMIT wheelPositionChanged(m_positionY);
        settle();
        return false;
    }

    double step = remaining * (1.0 - std::exp(-dtMs / kTauMs));
    const double minStep = kMinSettleSpeed * dtMs / 1000.0;
    if (std::abs(step) < minStep)
        step = std::copysign(std::min(minStep, absRemaining), remaining);

    // Never move a whole viewport in one frame (see kMaxStepViewportFraction).
    // m_targetY is untouched, so later frames cover the rest. Applied after the
    // minimum-speed floor so the tail cannot exceed the ceiling.
    const double maxStep = m_viewportHeight * kMaxStepViewportFraction;
    if (maxStep > 0.0 && std::abs(step) > maxStep)
        step = std::copysign(maxStep, remaining);

    m_positionY += step;
    Q_EMIT wheelPositionChanged(m_positionY);

    if (m_positionY == m_targetY) {
        settle();
        return false;
    }
    return true;
}

void TimelineScrollController::startTicker()
{
    if (!m_ticker)
        m_ticker = new WheelTicker(this);
    if (m_ticker->state() != QAbstractAnimation::Running)
        m_ticker->start();
}

void TimelineScrollController::stopTicker()
{
    // Re-entrancy guard: settle() can be reached from the ticker's own
    // updateCurrentTime.
    if (m_ticker && m_ticker->state() == QAbstractAnimation::Running)
        m_ticker->stop();
}

bool TimelineScrollController::tickerRunningForTest() const
{
    return m_ticker && m_ticker->state() == QAbstractAnimation::Running;
}

void TimelineScrollController::settle()
{
    stopTicker();
    m_direction = 0;
    setMotionActive(false);
    Q_EMIT wheelMotionSettled();
}

void TimelineScrollController::endMotion()
{
    stopTicker();
    m_direction = 0;
    setMotionActive(false);
}

void TimelineScrollController::cancel()
{
    stopTicker();
    m_direction = 0;
    setMotionActive(false);
}
