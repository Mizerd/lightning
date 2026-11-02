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
#include <cstdint>
#include <cstdio>
#include <limits>

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
#define LIGHTNING_IDLE_TRIM 1
#endif
#endif

namespace lightning {

// Hand the free memory glibc keeps inside its arenas back to the system while
// nobody is using the app.
//
// Measured after a long scroll and leaving the room: 705 MB sat free inside
// the arenas, held there by the few live blocks scattered through them, and
// malloc_trim(0) took 45-48 ms to return 208-419 MB of it. The trim cannot run
// while the user is scrolling (it takes every arena lock in turn), so it waits
// for quiet and runs on a pool thread, never the GUI thread.
inline constexpr qint64 kIdleTrimQuietMs = 60 * 1000;
inline constexpr qint64 kIdleTrimMinGapMs = 5 * 60 * 1000;
inline constexpr std::uint64_t kIdleTrimMinFreeBytes = 64ull * 1024 * 1024;

struct IdleTrimState
{
    qint64 msSinceInput = 0;
    bool windowHidden = false;
    int activeCalls = 0;
    // Free bytes inside the arenas; negative when the allocator cannot say.
    qint64 freeArenaBytes = -1;
    // Negative when no trim has run yet.
    qint64 msSinceLastTrim = -1;
};

// Pure: every condition must hold.
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

// Milliseconds until the first time shouldIdleTrim could turn true on time
// alone, or -1 when time cannot change the answer (a call, too little free
// memory), in which case nothing is armed until the next activity.
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

// Event-driven: a single-shot timer armed by input and room changes, and
// re-armed from its own handler only while there is something left to wait
// for. A process nobody touches wakes at most once after its last activity.
class IdleTrim : public QObject
{
public:
    explicit IdleTrim(QObject *parent = nullptr) : QObject(parent)
    {
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] { onTimeout(); });
        m_sinceInput.start();
    }

    // Room changes and anything else the user did without an input event here.
    void noteActivity()
    {
#if defined(LIGHTNING_IDLE_TRIM)
        m_sinceInput.restart();
        // A pending timer already fires late enough; it re-checks on fire.
        if (!m_timer.isActive())
            m_timer.start(static_cast<int>(kIdleTrimQuietMs));
#endif
    }

    // Free bytes in the arenas, or -1 when unavailable.
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
        s.msSinceLastTrim = m_haveTrimmed ? m_sinceTrim.elapsed() : -1;
        if (shouldIdleTrim(s)) {
            runTrim();
            return;
        }
        const qint64 wait = idleTrimWaitMs(s);
        if (wait >= 0)
            m_timer.start(static_cast<int>(qMax<qint64>(wait, 1000)));
#endif
    }

    void runTrim()
    {
#if defined(LIGHTNING_IDLE_TRIM)
        if (running().exchange(true))
            return;
        m_haveTrimmed = true;
        m_sinceTrim.restart();
        // Captures nothing: the job may outlive this object (and the stack
        // frame holding it) at quit, so it touches only static state.
        QThreadPool::globalInstance()->start([] {
            // A call may have started while the job waited for a thread.
            if (CallAllocatorHold::activeCalls() > 0) {
                running() = false;
                return;
            }
            const qint64 freeBefore = freeArenaBytes();
            if (freeBefore < 0
                || static_cast<std::uint64_t>(freeBefore) < kIdleTrimMinFreeBytes) {
                running() = false;
                return;
            }
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

    // Static, so a job still running at quit never reaches a dead object.
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
    QElapsedTimer m_sinceTrim;
    bool m_haveTrimmed = false;
};

} // namespace lightning
