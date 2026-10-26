#include "app/FrameIntervalProbe.h"

#include <QQuickWindow>
#include <QScreen>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

double FrameIntervalStats::percentile(double p) const
{
    if (m_samples.isEmpty())
        return 0.0;
    QVector<double> sorted = m_samples;
    std::sort(sorted.begin(), sorted.end());
    const double clamped = std::clamp(p, 0.0, 100.0);
    // Nearest rank: ceil(p/100 * n), 1-based.
    int rank = int(std::ceil(clamped / 100.0 * sorted.size()));
    rank = std::clamp(rank, 1, int(sorted.size()));
    return sorted.at(rank - 1);
}

double FrameIntervalStats::worst() const
{
    return m_samples.isEmpty()
        ? 0.0 : *std::max_element(m_samples.cbegin(), m_samples.cend());
}

int FrameIntervalStats::countOver(double ms) const
{
    return int(std::count_if(m_samples.cbegin(), m_samples.cend(),
                             [ms](double v) { return v > ms; }));
}

QString FrameIntervalStats::summary(double refreshHz, double windowSeconds) const
{
    const double hz = refreshHz > 1.0 ? refreshHz : 60.0;
    const double interval = 1000.0 / hz;
    return QStringLiteral(
               "frame-intervals n=%1 window_s=%2 refresh_hz=%3 p50=%4 p95=%5 "
               "p99=%6 max=%7 over1.25x=%8 over2x=%9")
        .arg(count())
        .arg(windowSeconds, 0, 'f', 1)
        .arg(hz, 0, 'f', 1)
        .arg(percentile(50), 0, 'f', 2)
        .arg(percentile(95), 0, 'f', 2)
        .arg(percentile(99), 0, 'f', 2)
        .arg(worst(), 0, 'f', 2)
        .arg(countOver(interval * 1.25))
        .arg(countOver(interval * 2.0));
}

bool FrameIntervalStats::continuousMotion(double refreshHz) const
{
    const double hz = refreshHz > 1.0 ? refreshHz : 60.0;
    return count() > 0 && percentile(50) <= 3.0 * 1000.0 / hz;
}

FrameIntervalProbe::FrameIntervalProbe(QQuickWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
    m_clock.start();
    if (window) {
        // Direct: stamp the swap on the thread that performed it.
        connect(window, &QQuickWindow::frameSwapped, this,
                [this] { onFrameSwapped(); }, Qt::DirectConnection);
    }
    m_reportTimer.setInterval(kReportMs);
    connect(&m_reportTimer, &QTimer::timeout, this, [this] { report(); });
    m_reportTimer.start();
}

bool FrameIntervalProbe::enabledByEnvironment()
{
    return qEnvironmentVariableIsSet("LIGHTNING_FRAME_TRACE");
}

void FrameIntervalProbe::onFrameSwapped()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const qint64 now = m_clock.nsecsElapsed();
    if (m_lastNs >= 0) {
        const double ms = double(now - m_lastNs) / 1e6;
        if (ms <= kIdleGapMs && m_stats.count() < kMaxSamples)
            m_stats.add(ms);
    }
    m_lastNs = now;
}

void FrameIntervalProbe::report()
{
    FrameIntervalStats window;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        window = m_stats;
        m_stats.clear();
    }
    if (window.count() < kMinFrames)
        return;
    double hz = 60.0;
    if (m_window && m_window->screen())
        hz = m_window->screen()->refreshRate();
    if (!window.continuousMotion(hz))
        return;
    qInfo().noquote() << window.summary(hz, kReportMs / 1000.0);
}
