#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

#include <mutex>

class QQuickWindow;

// Frame-interval statistics for the opt-in frame probe (master prompt Part 3
// §7, Part 5 §8): percentiles of the intervals between presented frames, the
// worst one, and how many exceeded 1.25 and 2 display intervals. Pure, so it
// is unit-tested without a window. Nearest-rank percentiles over the samples
// of one reporting window; no averages, which hide a rare long frame.
class FrameIntervalStats
{
public:
    void add(double intervalMs) { m_samples.append(intervalMs); }
    int count() const { return int(m_samples.size()); }
    void clear() { m_samples.clear(); }
    // p in (0, 100]; 0 when empty.
    double percentile(double p) const;
    double worst() const;
    int countOver(double ms) const;
    // One log line: n, p50/p95/p99/max, and the counts over 1.25x and 2x of
    // the display interval for `refreshHz` (60 when unknown).
    QString summary(double refreshHz, double windowSeconds) const;
    // True when the window's frames were mostly a continuous animation (its
    // median within 3 display intervals). A slow periodic repaint (a blinking
    // caret, a 5 Hz indicator) presents frames 100-250 ms apart all the time;
    // summarising those as "every frame over 2x" measured the idle UI, not
    // motion (live, wave-2 GUI round).
    bool continuousMotion(double refreshHz) const;

private:
    QVector<double> m_samples;
};

// Opt-in (LIGHTNING_FRAME_TRACE): records the interval between consecutive
// frameSwapped signals of one window and logs a FrameIntervalStats summary
// every reporting window in which enough frames were presented to mean
// something (the scene graph renders only on change, so an idle app presents
// almost nothing and that is not a measurement). Intervals longer than
// kIdleGapMs are the end of a burst of animation, not a slow frame, and are
// dropped. frameSwapped arrives on the render thread: the timestamp is taken
// there and the samples handed over under a mutex; nothing is logged per frame.
// A measurement of presentation cadence as Qt sees it, not compositor
// presentation, and not a substitute for QSG_RENDER_TIMING's stage split.
class FrameIntervalProbe : public QObject
{
    Q_OBJECT
public:
    static constexpr int kReportMs = 10000;
    static constexpr int kMinFrames = 30;
    static constexpr double kIdleGapMs = 250.0;
    static constexpr int kMaxSamples = 20000; // bounded per reporting window

    explicit FrameIntervalProbe(QQuickWindow *window, QObject *parent = nullptr);
    static bool enabledByEnvironment();

private:
    void onFrameSwapped();   // render thread
    void report();           // GUI thread

    QPointer<QQuickWindow> m_window;
    QTimer m_reportTimer;
    std::mutex m_mutex;
    QElapsedTimer m_clock;
    qint64 m_lastNs = -1;
    FrameIntervalStats m_stats;
};
