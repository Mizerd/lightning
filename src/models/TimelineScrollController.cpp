#include "models/TimelineScrollController.h"

#include <QAbstractAnimation>

#include <algorithm>
#include <cmath>

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

// Motion engine tuning. The exponential time constant sets responsiveness:
// each frame covers (1 - exp(-dt/tau)) of the remaining distance, so speed is
// proportional to what is left — high right after a notch, decaying smoothly,
// and RISING smoothly again when another same-direction notch extends the
// target. The minimum settle speed bounds the exponential tail so motion
// stops crisply (a pure exponential never quite arrives); the snap distance
// ends it exactly. A dt clamp keeps a stalled frame (GC, window move) from
// integrating one giant visible jump.
constexpr double kTauMs = 90.0;
constexpr double kMinSettleSpeed = 700.0;   // px/s
constexpr double kSnapDistance = 0.5;       // px
constexpr double kMaxTickMs = 50.0;

// HARD CEILING on how far ONE emitted frame may move contentY, as a fraction
// of the viewport height. This is not a feel tuning knob — it exists because
// of a specific, verified QQuickTableView behaviour.
//
// QQuickTableView::viewportMoved() -> QQuickTableViewPrivate::
// scheduleRebuildIfFastFlick() (qquicktableview.cpp, Qt 6.11.1) tests whether
// the NEW viewport rect still intersects the one cached at the last layout:
//
//     if (!viewportRect.intersects(QRectF(viewportRect.x(), q->contentY(),
//                                         1, q->height())))
//         scheduledRebuildOptions |= RebuildOption::CalculateNewTopLeftRow
//                                  | RebuildOption::ViewportOnly;
//
// So ANY single contentY write of a whole viewport or more makes TableView
// discard its current top row and GUESSTIMATE a replacement in
// calculateTopLeft():
//
//     newRow = int(viewportRect.y() / (averageEdgeSize.height() + spacing))
//     topLeftPos.y = newRow * (averageEdgeSize.height() + spacing)
//
// averageEdgeSize.height() is contentHeight / rowCount — ONE uniform average.
// A message timeline has no uniform row height: a collapsed state line is
// ~28 px and a video card ~400 px. Applied to a reader tens of thousands of
// pixels into loaded history, that guess resolves to a row nowhere near the
// one being read, and the table is rebuilt around it. The reader sees a
// different part of the conversation for a frame or two until the anchor
// machinery drags them back — the reported "jitter that loses the message I
// was reading".
//
// This bites hardest exactly when older history lands: the frame that inserts
// and lays out a page is long, wheel notches keep coalescing into the target
// during it, and the tick after the stall integrates all of that accumulated
// distance in one emission. VeryFast reaches 1000 px per notch, dt is capped
// at 50 ms == 42% of the remaining distance, so a handful of notches already
// clears a viewport in a single frame.
//
// Half a viewport keeps the rects overlapping with margin to spare (QML may
// write contentY more than once between two of TableView's layout passes),
// and costs only a few extra frames on the largest glides — the exponential
// curve means the cap is only reached at the very start of a big motion,
// where velocity is highest and one more frame is invisible.
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

// Frame source for the motion engine: driven by Qt's unified animation
// driver (vsync-aligned inside a Qt Quick application), started only while
// motion is in flight and stopped the moment it settles — never a
// permanently running timer.
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

void TimelineScrollController::wheelNotch(double angleDeltaY, double contentY,
                                          double minContentY, double maxContentY,
                                          double viewportHeight)
{
    // Mid-motion the engine's integrated position is authoritative (QML's
    // contentY may lag by the frame in flight); otherwise seed from the view.
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
    // Ignore a non-positive height (the pane before it has a size) rather
    // than disabling the step cap for the motion that follows.
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
    // The platform's own momentum arrives as a stream of pixel-delta events;
    // applying them directly preserves that momentum without a competing
    // animation. Drop any coalesced wheel goal so the two paths never fight.
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
    // Same body as advanceMotion's per-frame maths, with no state: the
    // exponential approach, the minimum-settle-speed floor that stops the
    // tail crawling, and the per-frame ceiling that stops a large remaining
    // distance jumping a whole viewport.
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
    // Nothing in flight to translate — the caller cancels instead. A zero
    // shift (e.g. a pagination batch that inserted no visible rows) is also
    // a no-op, deliberately, rather than a redundant identical write.
    if (!m_motionActive || deltaY == 0.0)
        return;
    m_positionY += deltaY;
    m_targetY += deltaY;
    // Remaining distance (m_targetY - m_positionY) is unchanged by
    // construction, so the ticker's next advanceMotion() continues the same
    // deceleration curve toward the relocated target — no visible restart,
    // no lost momentum. QML's onWheelPositionChanged reclamps every emitted
    // frame against live geometry, so an out-of-range target here is caught
    // there rather than needing its own clamp.
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

    // Never emit a frame that moves the view a whole viewport or more; see
    // kMaxStepViewportFraction. The remainder is not lost — m_targetY is
    // untouched, so the following frames continue toward it on the same
    // curve. Applied after the minimum-speed floor so the settle tail can
    // never be re-inflated past the ceiling.
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
    // Guard against re-entrancy: settle() can be reached from inside the
    // ticker's own updateCurrentTime (which stops itself via the advance
    // return value).
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
