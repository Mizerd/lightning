#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

class QAbstractAnimation;

class TimelineScrollController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("TimelineScrollController is exposed via app.timelineScroll")
    Q_PROPERTY(WheelSpeed wheelSpeed READ wheelSpeed WRITE setWheelSpeed
                   NOTIFY wheelSpeedChanged)
    Q_PROPERTY(bool motionActive READ motionActive NOTIFY motionActiveChanged)
    Q_PROPERTY(bool scrollTraceEnabled READ scrollTraceEnabled CONSTANT)
    Q_PROPERTY(bool scrollRecorderEnabled READ scrollRecorderEnabled CONSTANT)
    Q_PROPERTY(int scrollIncidentCount READ scrollIncidentCount
                   NOTIFY scrollIncidentCountChanged)

public:
    enum WheelSpeed { Standard = 0, Fast = 1, VeryFast = 2 };
    Q_ENUM(WheelSpeed)

    static constexpr int kMinSpeed = Standard;
    static constexpr int kMaxSpeed = VeryFast;

    explicit TimelineScrollController(QObject *parent = nullptr);

    WheelSpeed wheelSpeed() const { return m_wheelSpeed; }
    void setWheelSpeed(WheelSpeed speed);
    Q_INVOKABLE void setWheelSpeedValue(int value);

    bool motionActive() const { return m_motionActive; }
    bool scrollTraceEnabled() const { return m_scrollTraceEnabled; }
    bool scrollRecorderEnabled() const { return m_recorderEnabled; }
    int scrollIncidentCount() const { return m_incidentCount; }
    static constexpr int kRecorderCapacity = 1024;
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
    Q_INVOKABLE bool reportScrollIncident(const QString &code,
                                          int records = 64);
    Q_INVOKABLE int recordedScrollCount() const { return m_ringSize; }
    Q_INVOKABLE QString lastScrollIncidentDump() const { return m_lastDump; }
    void setScrollRecorderEnabledForTest(bool on) { m_recorderEnabled = on; }
    static constexpr int kIncidentMinGapMs = 2000;
    static constexpr int kMaxAnchorAliases = 4096;

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

    Q_INVOKABLE void animateTo(double targetY, double contentY,
                               double minContentY, double maxContentY,
                               double viewportHeight = 0.0);

    Q_INVOKABLE double wheelTargetY(double angleDeltaY, double contentY,
                                    double minContentY, double maxContentY,
                                    double viewportHeight);

    Q_INVOKABLE double pixelTargetY(double pixelDeltaY, double contentY,
                                    double minContentY, double maxContentY);

    Q_INVOKABLE void notifyBoundReached(double clampedY);

    // A backward-pagination prepend shifted content during a wheel glide: shift
    // both the position and the target by deltaY so the glide continues instead
    // of being cancelled. No-op without motion in flight (the caller then uses
    // cancel()). The next frame is re-clamped as usual.
    Q_INVOKABLE void translateActiveMotion(double deltaY);

    Q_INVOKABLE double motionStep(double remaining, double dtMs,
                                  double viewportHeight) const;

    Q_INVOKABLE void endMotion();
    // Hard cancel: room/account change, programmatic navigation, destruction.
    // Stops the ticker without emitting a final position or settle signal —
    // the caller owns contentY from here.
    Q_INVOKABLE void cancel();

    bool advanceMotion(double dtMs);

    Q_INVOKABLE void setViewportHeight(double viewportHeight);

    void setPixelFactorForTest(double factor) { m_pixelFactor = factor; }
    double targetYForTest() const { return m_targetY; }
    double positionYForTest() const { return m_positionY; }
    bool tickerRunningForTest() const;

Q_SIGNALS:
    void scrollIncidentCountChanged();
    void wheelSpeedChanged();
    void motionActiveChanged();
    void wheelPositionChanged(double contentY);
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

    double m_targetY = 0.0;
    double m_positionY = 0.0;
    int m_direction = 0;
    bool m_motionActive = false;

    double m_minY = 0.0;
    double m_maxY = 0.0;

    double m_viewportHeight = 0.0;

    double m_pixelFactor = 1.0;

    bool m_scrollTraceEnabled = false;

    struct ScrollRecord {
        qint64 ms = 0;
        quint32 seq = 0;
        quint8 writer = 0;
        quint8 flags = 0;
        float oldY = 0, newY = 0, minY = 0, maxY = 0, viewport = 0,
              content = 0, anchorOffset = 0;
        qint32 count = 0, windowSkip = 0, anchor = -1;
        quint8 anchorKind = 0;
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

    QAbstractAnimation *m_ticker = nullptr;
};
