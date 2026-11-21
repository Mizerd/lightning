#pragma once

#include "app/AllocatorPolicy.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
#include <QtLogging>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
#define LIGHTNING_IDLE_TRIM 1
#endif
#endif

namespace lightning {

inline constexpr qint64 kIdleTrimQuietMs = 60 * 1000;
inline constexpr qint64 kIdleTrimMinGapMs = 5 * 60 * 1000;
inline constexpr std::uint64_t kIdleTrimMinFreeBytes = 64ull * 1024 * 1024;

struct IdleTrimState
{
    qint64 msSinceInput = 0;
    bool windowHidden = false;
    int activeCalls = 0;
    qint64 freeArenaBytes = -1;
    qint64 msSinceLastTrim = -1;
};

inline bool shouldIdleTrim(const IdleTrimState &s)
{
    if (s.activeCalls > 0)
        return false;
    if (s.freeArenaBytes < 0
        || static_cast<std::uint64_t>(s.freeArenaBytes) < kIdleTrimMinFreeBytes)
        return false;
    if (s.msSinceLastTrim >= 0 && s.msSinceLastTrim < kIdleTrimMinGapMs)
        return false;
    return s.windowHidden || s.msSinceInput >= kIdleTrimQuietMs;
}

inline qint64 idleTrimWaitMs(const IdleTrimState &s)
{
    if (s.activeCalls > 0 || s.freeArenaBytes < 0
        || static_cast<std::uint64_t>(s.freeArenaBytes) < kIdleTrimMinFreeBytes)
        return -1;
    qint64 wait = s.windowHidden ? 0 : kIdleTrimQuietMs - s.msSinceInput;
    if (s.msSinceLastTrim >= 0)
        wait = qMax(wait, kIdleTrimMinGapMs - s.msSinceLastTrim);
    return qMax<qint64>(wait, 0);
}

class IdleTrim : public QObject
{
public:
    explicit IdleTrim(QObject *parent = nullptr) : QObject(parent)
    {
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] { onTimeout(); });
        m_sinceInput.start();
#if defined(LIGHTNING_IDLE_TRIM)
        m_timer.start(static_cast<int>(kIdleTrimQuietMs));
#endif
    }

    void noteActivity()
    {
        m_sinceInput.restart();
#if defined(LIGHTNING_IDLE_TRIM)
        if (!m_timer.isActive())
            m_timer.start(static_cast<int>(kIdleTrimQuietMs));
#endif
    }

    qint64 msSinceInput() const { return m_sinceInput.elapsed(); }

    // Arms a check without counting as input, for memory released by the app itself.
    void requestTrim()
    {
#if defined(LIGHTNING_IDLE_TRIM)
        if (!m_timer.isActive() || m_timer.remainingTime() > 5000)
            m_timer.start(5000);
#endif
    }

    bool checkArmed() const { return m_timer.isActive(); }

    static qint64 freeArenaBytes()
    {
#if defined(LIGHTNING_IDLE_TRIM)
        const struct mallinfo2 mi = mallinfo2();
        return static_cast<qint64>(mi.fordblks);
#else
        return -1;
#endif
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
            noteActivity();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

    // Always re-arms, so an idle session keeps being checked without input.
    void onTimeout()
    {
#if defined(LIGHTNING_IDLE_TRIM)
        IdleTrimState s;
        s.msSinceInput = m_sinceInput.elapsed();
        s.windowHidden = !anyWindowShown();
        s.activeCalls = CallAllocatorHold::activeCalls();
        // mallinfo2 walks every arena under its lock, which is exactly what
        // must not happen on the GUI thread under load. The cheap conditions
        // decide here; the free-bytes floor is checked by the job, on the pool
        // thread, immediately before the trim.
        s.freeArenaBytes = std::numeric_limits<qint64>::max();
        const qint64 last = lastTrimAtMs().load();
        s.msSinceLastTrim = last < 0 ? -1 : nowMs() - last;
        qint64 wait = kIdleTrimMinGapMs;
        if (shouldIdleTrim(s))
            runTrim();
        else if (const qint64 w = idleTrimWaitMs(s); w >= 0)
            wait = qMax<qint64>(w, 1000);
        m_timer.start(static_cast<int>(wait));
#endif
    }

private:
    static bool anyWindowShown()
    {
        for (QWindow *w : QGuiApplication::topLevelWindows()) {
            if (w->isVisible() && w->visibility() != QWindow::Minimized
                && w->visibility() != QWindow::Hidden)
                return true;
        }
        return false;
    }

    void runTrim()
    {
#if defined(LIGHTNING_IDLE_TRIM)
        if (running().exchange(true))
            return;
        QThreadPool::globalInstance()->start([] {
            if (CallAllocatorHold::activeCalls() > 0) {
                running() = false;
                return;
            }
            const qint64 freeBefore = freeArenaBytes();
            if (freeBefore < 0
                || static_cast<std::uint64_t>(freeBefore) < kIdleTrimMinFreeBytes) {
                qDebug("idle trim: skipped, free in arenas %lld MB is below the floor",
                       static_cast<long long>(freeBefore >> 20));
                running() = false;
                return;
            }
            lastTrimAtMs() = nowMs();
            const qint64 rssBefore = residentBytes();
            const int released = malloc_trim(0);
            const qint64 rssAfter = residentBytes();
            qInfo("idle trim: free in arenas %lld MB, resident %lld -> %lld MB, returned %lld bytes (released=%d)",
                  static_cast<long long>(freeBefore >> 20),
                  static_cast<long long>(rssBefore >> 20),
                  static_cast<long long>(rssAfter >> 20),
                  static_cast<long long>(qMax<qint64>(rssBefore - rssAfter, 0)), released);
            running() = false;
        });
#endif
    }

    static std::atomic<qint64> &lastTrimAtMs()
    {
        static std::atomic<qint64> at{-1};
        return at;
    }

    static qint64 nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static std::atomic<bool> &running()
    {
        static std::atomic<bool> flag{false};
        return flag;
    }

    static qint64 residentBytes()
    {
        std::FILE *f = std::fopen("/proc/self/statm", "r");
        if (!f)
            return 0;
        long total = 0, resident = 0;
        const int n = std::fscanf(f, "%ld %ld", &total, &resident);
        std::fclose(f);
        return n == 2 ? static_cast<qint64>(resident) * 4096 : 0;
    }

    QTimer m_timer;
    QElapsedTimer m_sinceInput;
};

}
