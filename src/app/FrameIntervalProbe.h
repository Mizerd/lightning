#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

#include <mutex>

class QQuickWindow;

class FrameIntervalStats
{
public:
    void add(double intervalMs) { m_samples.append(intervalMs); }
    int count() const { return int(m_samples.size()); }
    void clear() { m_samples.clear(); }
    double percentile(double p) const;
    double worst() const;
    int countOver(double ms) const;
    QString summary(double refreshHz, double windowSeconds) const;
    bool continuousMotion(double refreshHz) const;

private:
    QVector<double> m_samples;
};

class FrameIntervalProbe : public QObject
{
    Q_OBJECT
public:
    static constexpr int kReportMs = 10000;
    static constexpr int kMinFrames = 30;
    static constexpr double kIdleGapMs = 250.0;
    static constexpr int kMaxSamples = 20000;

    explicit FrameIntervalProbe(QQuickWindow *window, QObject *parent = nullptr);
    static bool enabledByEnvironment();

private:
    void onFrameSwapped();   // render thread
    void report();

    QPointer<QQuickWindow> m_window;
    QTimer m_reportTimer;
    std::mutex m_mutex;
    QElapsedTimer m_clock;
    qint64 m_lastNs = -1;
    FrameIntervalStats m_stats;
};
