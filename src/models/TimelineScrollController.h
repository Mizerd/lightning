#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

class QAbstractAnimation;

// Timeline wheel-scroll policy and motion engine.
//
// Owns the scroll math so it can be unit-tested deterministically (the
// offscreen QPA used by QML tests cannot exercise real view geometry). A
// wheel-only ticker (a QAbstractAnimation, running only while motion is
// active) integrates the position toward the coalesced target each frame:
//
//   step = remaining * (1 - exp(-dt/tau))        // exponential approach
//   |step| >= minSettleSpeed * dt                // bounded tail, quick stop
//
// Velocity stays continuous within a gesture (restarting a fixed-duration
// animation per notch produced a sawtooth), same-direction notches extend the
// target, a reversal redirects immediately, and motion settles in bounded time
// with no momentum tail. QML applies emitted positions to contentY and
// re-clamps against live geometry.
//
// Input sources:
//   * angle-delta mouse wheel -> wheelNotch(): a bounded per-notch distance
//     from the viewport and speed, coalesced into one motion;
//   * pixel-delta touchpad / precision wheel -> pixelTargetY(): applied
//     directly with mild scaling, preserving native momentum;
//   * programmatic navigation is not routed here; QML calls cancel() first.
//     The exception is a backward-pagination anchor restore during a glide,
//     which uses translateActiveMotion() so momentum survives the prepend.
//
// Never logs, except the opt-in scroll incident recorder below
// (LIGHTNING_SCROLL_RECORDER or LIGHTNING_SCROLL_TRACE), which carries no
// message content, room ids, event ids or URLs either.
class TimelineScrollController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Exposed to QML only as the "app.timelineScroll" context property; the
    // registration lets QML name the WheelSpeed enum.
    QML_UNCREATABLE("TimelineScrollController is exposed via app.timelineScroll")
    Q_PROPERTY(WheelSpeed wheelSpeed READ wheelSpeed WRITE setWheelSpeed
                   NOTIFY wheelSpeedChanged)
    // True while the wheel motion engine owns the viewport, so QML can treat
    // programmatic contentY changes as user-intent for follow-latest /
    // pagination without waiting for Flickable.moving.
    Q_PROPERTY(bool motionActive READ motionActive NOTIFY motionActiveChanged)
    // Per-gesture scroll diagnostics, enabled by LIGHTNING_SCROLL_TRACE. When
    // on, TimelinePane logs one summary line per gesture at settle: event
    // count, device mix, net movement, content-height churn, and how many times
    // a deferred anchor correction wrote the position mid-gesture (must be 0).
    // Read once at construction; no message content or ids.
    Q_PROPERTY(bool scrollTraceEnabled READ scrollTraceEnabled CONSTANT)
    // Bounded causal recorder for a rare viewport jump (master prompt Part 3
    // §4): every main-timeline position write is recorded with its writer
    // reason, bounds, row-window state and a SESSION-LOCAL anchor alias (an
    // integer, never the event id) in a fixed ring of kRecorderCapacity
    // records. Nothing is printed until reportScrollIncident() names an
    // anomaly, which dumps the most recent records once (rate-limited).
    // Opt-in: LIGHTNING_SCROLL_RECORDER or LIGHTNING_SCROLL_TRACE; off, the
    // QML side does not even call in.
    Q_PROPERTY(bool scrollRecorderEnabled READ scrollRecorderEnabled CONSTANT)
    Q_PROPERTY(int scrollIncidentCount READ scrollIncidentCount
                   NOTIFY scrollIncidentCountChanged)

public:
    // Persisted as a stable integer (see SettingsManager). Ordered by per-notch
    // distance.
    enum WheelSpeed { Standard = 0, Fast = 1, VeryFast = 2 };
    Q_ENUM(WheelSpeed)

    static constexpr int kMinSpeed = Standard;
    static constexpr int kMaxSpeed = VeryFast;

    explicit TimelineScrollController(QObject *parent = nullptr);

    WheelSpeed wheelSpeed() const { return m_wheelSpeed; }
    void setWheelSpeed(WheelSpeed speed);
    // For the settings bridge, which stores an int. Out-of-range values fall
    // back to Fast.
    Q_INVOKABLE void setWheelSpeedValue(int value);

    bool motionActive() const { return m_motionActive; }
    bool scrollTraceEnabled() const { return m_scrollTraceEnabled; }
    bool scrollRecorderEnabled() const { return m_recorderEnabled; }
    int scrollIncidentCount() const { return m_incidentCount; }
    static constexpr int kRecorderCapacity = 1024;
    // Writer reasons the recorder knows; anything else records as "other".
    // Bit flags for recordScroll's `flags`.
    enum RecorderFlag {
        UserScrollActive = 1, FollowLatest = 2, WheelActive = 4,
        SettleActive = 8, NavigationPending = 16, RestoreHold = 32,
        ScrollbarPressed = 64, Dragging = 128
    };
    Q_ENUM(RecorderFlag)
    Q_INVOKABLE void recordScroll(const QString &writer, double oldY,
                                  double newY, double minY, double maxY,
                                  double viewportHeight, double contentHeight,
                                  int count, int windowSkip, int flags,
                                  const QString &anchorId,
                                  double anchorOffset,
                                  const QString &anchorKind = QString(),
                                  int anchorFromOldest = -1);
    // Dumps the last `records` records (bounded by the ring) under the
    // incident `code` (a fixed vocabulary: oldest-bound, newest-bound,
    // missing-anchor, stale-generation, ...). At most one dump per
    // kIncidentMinGapMs; the count still advances. Returns whether it dumped.
    Q_INVOKABLE bool reportScrollIncident(const QString &code,
                                          int records = 64);
    Q_INVOKABLE int recordedScrollCount() const { return m_ringSize; }
    // Test/diagnostic: the text the last dump produced (also logged).
    Q_INVOKABLE QString lastScrollIncidentDump() const { return m_lastDump; }
    // For tests: enables the recorder regardless of the environment.
    void setScrollRecorderEnabledForTest(bool on) { m_recorderEnabled = on; }
    static constexpr int kIncidentMinGapMs = 2000;
    static constexpr int kMaxAnchorAliases = 4096;

    // Pixels one full notch (angleDelta.y == 120) scrolls at the active speed
    // for this viewport height, bounded at both ends.
    Q_INVOKABLE double notchDistance(double viewportHeight) const;
    Q_INVOKABLE double notchDistanceForSpeed(int speed, double viewportHeight) const;

    // Discrete wheel input: updates the coalesced target and engages the
    // engine. angleDeltaY > 0 means wheel up (toward older content, contentY
    // decreases). Same-direction input extends the target; a reversal redirects
    // from the live position; partial deltas contribute proportionally.
    // contentY seeds the position only when no motion is in flight.
    Q_INVOKABLE void wheelNotch(double angleDeltaY, double contentY,
                                double minContentY, double maxContentY,
                                double viewportHeight);

    // Smooth motion to an absolute target (Page Up/Down, Space) on the same
    // engine, so repeated presses coalesce. viewportHeight feeds the per-frame
    // step cap; omitted, the last known height is kept.
    Q_INVOKABLE void animateTo(double targetY, double contentY,
                               double minContentY, double maxContentY,
                               double viewportHeight = 0.0);

    // Pure target computation for the coalescing policy (also used by
    // wheelNotch), clamped to [minContentY, maxContentY].
    Q_INVOKABLE double wheelTargetY(double angleDeltaY, double contentY,
                                    double minContentY, double maxContentY,
                                    double viewportHeight);

    // Pixel-delta input: returns the contentY to jump to, with mild bounded
    // scaling, never the notch multiplier. Cancels any coalesced wheel target,
    // since the platform owns momentum here.
    Q_INVOKABLE double pixelTargetY(double pixelDeltaY, double contentY,
                                    double minContentY, double maxContentY);

    // QML re-clamped an emitted position against live geometry and hit a bound:
    // adopt it and settle.
    Q_INVOKABLE void notifyBoundReached(double clampedY);

    // A backward-pagination prepend shifted content during a wheel glide: shift
    // both the position and the target by deltaY so the glide continues instead
    // of being cancelled. No-op without motion in flight (the caller then uses
    // cancel()). The next frame is re-clamped as usual.
    Q_INVOKABLE void translateActiveMotion(double deltaY);

    // One frame of this controller's deceleration for a glide with `remaining`
    // distance in a `viewportHeight` view. Pure; lets SmoothWheelArea drive an
    // independent glide on the identical curve (a QML easing curve felt like
    // scrolling in blocks).
    Q_INVOKABLE double motionStep(double remaining, double dtMs,
                                  double viewportHeight) const;

    // Explicit end of motion (tests); the engine normally settles itself.
    Q_INVOKABLE void endMotion();
    // Hard cancel: room/account change, programmatic navigation, destruction.
    // Stops the ticker without emitting a final position or settle signal —
    // the caller owns contentY from here.
    Q_INVOKABLE void cancel();

    // Engine step, public so the ticker and deterministic tests share the same
    // integration. Returns false once motion has settled.
    bool advanceMotion(double dtMs);

    // Viewport height for the step cap; for keyboard/test paths that drive the
    // engine without a wheel notch.
    Q_INVOKABLE void setViewportHeight(double viewportHeight);

    // Test hooks.
    void setPixelFactorForTest(double factor) { m_pixelFactor = factor; }
    double targetYForTest() const { return m_targetY; }
    double positionYForTest() const { return m_positionY; }
    bool tickerRunningForTest() const;

Q_SIGNALS:
    void scrollIncidentCountChanged();
    void wheelSpeedChanged();
    void motionActiveChanged();
    // One frame of wheel motion: QML applies this to contentY (re-clamping
    // against live geometry).
    void wheelPositionChanged(double contentY);
    // Motion reached its target or a bound. QML recomputes follow-latest /
    // pagination and saves the anchor once.
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

    // Viewport height from the last motion request; 0 disables the step cap
    // rather than guessing.
    double m_viewportHeight = 0.0;

    // Touchpad pixel scaling. 1.0 == native; kept mild and tunable.
    double m_pixelFactor = 1.0;

    // Scroll diagnostics gate (LIGHTNING_SCROLL_TRACE), read once.
    bool m_scrollTraceEnabled = false;

    struct ScrollRecord {
        qint64 ms = 0;
        quint32 seq = 0;
        quint8 writer = 0;
        quint8 flags = 0;
        float oldY = 0, newY = 0, minY = 0, maxY = 0, viewport = 0,
              content = 0, anchorOffset = 0;
        qint32 count = 0, windowSkip = 0, anchor = -1;
        // Index into kAnchorKinds; 0 is "-" (none or unknown).
        quint8 anchorKind = 0;
        // Rows between the anchor and the oldest loaded row (0 = the anchor
        // IS the oldest row); -1 unknown.
        qint32 anchorFromOldest = -1;
    };
    bool m_recorderEnabled = false;
    QVector<ScrollRecord> m_ring;
    int m_ringHead = 0;
    int m_ringSize = 0;
    quint32 m_recordSeq = 0;
    QElapsedTimer m_recordClock;
    QHash<QString, qint32> m_anchorAliases;
    qint32 m_nextAnchorAlias = 0;
    qint64 m_lastIncidentMs = -1;
    int m_incidentCount = 0;
    QString m_lastDump;

    // Frame ticker; parented to this, running only while motion is active.
    QAbstractAnimation *m_ticker = nullptr;
};
