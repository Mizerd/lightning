#include "accounts/SessionScheduler.h"

#include <QDateTime>
#include <QFile>
#include <QLoggingCategory>
#include <QRandomGenerator>

#include <algorithm>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#endif

Q_LOGGING_CATEGORY(lcScheduler, "lightning.accounts.scheduler")

#if defined(Q_OS_WIN)
namespace {

// The resume probe's two Windows clocks, resolved at runtime from kernel32
// (Vista and 7 respectively) so no _WIN32_WINNT floor can hide their
// declarations, as WindowCaptureSrc resolves its own. Both or neither.
struct WinClocks {
    using TickCount64Fn = unsigned long long(WINAPI *)();
    using UnbiasedInterruptTimeFn = BOOL(WINAPI *)(unsigned long long *);
    TickCount64Fn tickCount64 = nullptr;
    UnbiasedInterruptTimeFn unbiasedInterruptTime = nullptr;
    bool usable() const { return tickCount64 && unbiasedInterruptTime; }
};

const WinClocks &winClocks()
{
    static const WinClocks clocks = [] {
        WinClocks c;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            c.tickCount64 = reinterpret_cast<WinClocks::TickCount64Fn>(
                reinterpret_cast<void *>(GetProcAddress(kernel, "GetTickCount64")));
            c.unbiasedInterruptTime = reinterpret_cast<WinClocks::UnbiasedInterruptTimeFn>(
                reinterpret_cast<void *>(GetProcAddress(kernel, "QueryUnbiasedInterruptTime")));
        }
        return c;
    }();
    return clocks;
}

} // namespace
#endif

namespace lightning::accounts {

// ---- CatchUpTracker -------------------------------------------------------------

void CatchUpTracker::begin(qint64 nowMs, quint64 sessionStateVersion)
{
    m_startedMs = nowMs;
    m_baseVersion = sessionStateVersion;
    m_samples.clear();
}

void CatchUpTracker::sample(qint64 nowMs, quint64 notableUpdatesTotal)
{
    if (!active())
        return;
    m_samples.append({nowMs, notableUpdatesTotal});
    // Keep one sample older than the window, so the rate always spans it.
    while (m_samples.size() > 2 && m_samples.at(1).first <= nowMs - kRateWindowMs)
        m_samples.removeFirst();
}

bool CatchUpTracker::complete(qint64 nowMs, bool initialSyncDone, bool roomListRunning,
                              quint64 sessionStateVersion) const
{
    if (!active())
        return true;
    if (nowMs - m_startedMs >= kCapMs)
        return true;
    // A "running" carried over from before this catch-up began says nothing:
    // the state must have moved since.
    if (!initialSyncDone || !roomListRunning || sessionStateVersion == m_baseVersion)
        return false;
    if (m_samples.size() < 2)
        return false;
    const auto &oldest = m_samples.first();
    const auto &newest = m_samples.last();
    const qint64 span = newest.first - oldest.first;
    if (span < kRateWindowMs)
        return false;
    const double rate = double(newest.second - oldest.second) * 1000.0 / double(span);
    return rate < kMaxNotablePerSecond;
}

void CatchUpTracker::reset()
{
    m_startedMs = -1;
    m_baseVersion = 0;
    m_samples.clear();
}

// ---- SessionScheduler -------------------------------------------------------------

SessionScheduler::SessionScheduler(std::function<qint64()> monotonicMs,
                                   std::function<double()> jitterUnit, QObject *parent)
    : QObject(parent)
    , m_monotonicMs(std::move(monotonicMs))
    , m_jitterUnit(std::move(jitterUnit))
{
    m_clock.start();
    m_probe.setInterval(kProbeIntervalMs);
    m_probe.setTimerType(Qt::PreciseTimer);
    connect(&m_probe, &QTimer::timeout, this, &SessionScheduler::probeTick);
}

qint64 SessionScheduler::now() const
{
    return m_monotonicMs ? m_monotonicMs() : m_clock.elapsed();
}

void SessionScheduler::setSuspendAwareClock(std::function<qint64()> suspendAwareMs)
{
    m_suspendAwareMs = std::move(suspendAwareMs);
    if (m_lastProbeMs >= 0) {
        m_lastProbeSuspendAwareMs = suspendAwareNow();
        m_lastProbeAwakeMs = awakeNow();
    }
}

qint64 SessionScheduler::suspendAwareNow() const
{
    if (m_suspendAwareMs)
        return m_suspendAwareMs();
#if defined(Q_OS_LINUX) || defined(Q_OS_DARWIN)
    // Linux's CLOCK_BOOTTIME and Darwin's CLOCK_MONOTONIC both keep counting
    // while the machine sleeps; neither is ever set back.
#if defined(Q_OS_LINUX)
    constexpr clockid_t kClock = CLOCK_BOOTTIME;
#else
    constexpr clockid_t kClock = CLOCK_MONOTONIC;
#endif
    struct timespec ts {};
    if (clock_gettime(kClock, &ts) == 0)
        return qint64(ts.tv_sec) * 1000 + qint64(ts.tv_nsec) / 1000000;
#endif
#if defined(Q_OS_WIN)
    // Interrupt time WITH the time spent asleep or hibernating; paired with
    // QueryUnbiasedInterruptTime in awakeNow(), never with QElapsedTimer,
    // whose QueryPerformanceCounter may count sleep too (review round 2).
    if (const WinClocks &clocks = winClocks(); clocks.usable())
        return qint64(clocks.tickCount64());
#endif
    // Elsewhere the wall clock: it counts sleep, and a clock set forward by
    // over 10 s reads as a resume, which only makes every host catch up.
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 SessionScheduler::awakeNow() const
{
    // An injected suspend-aware clock is measured against the injected (or
    // QElapsedTimer) monotonic one: the tests script both.
    if (m_suspendAwareMs)
        return now();
#if defined(Q_OS_WIN)
    // Interrupt time WITHOUT sleep and hibernation, in 100 ns units: its gap
    // against GetTickCount64's is the time suspended, whatever QPC does.
    if (const WinClocks &clocks = winClocks(); clocks.usable()) {
        unsigned long long unbiased = 0;
        if (clocks.unbiasedInterruptTime(&unbiased))
            return qint64(unbiased / 10000);
    }
#endif
    return now();
}

double SessionScheduler::unit() const
{
    const double u = m_jitterUnit ? m_jitterUnit() : QRandomGenerator::global()->generateDouble();
    return std::clamp(u, 0.0, 1.0);
}

void SessionScheduler::startProbe()
{
    m_lastProbeMs = now();
    m_lastProbeSuspendAwareMs = suspendAwareNow();
    m_lastProbeAwakeMs = awakeNow();
    m_probe.start();
}

void SessionScheduler::stopProbe()
{
    m_probe.stop();
    m_lastProbeMs = -1;
    m_lastProbeSuspendAwareMs = -1;
    m_lastProbeAwakeMs = -1;
}

void SessionScheduler::probeTick()
{
    const qint64 t = now();
    const qint64 s = suspendAwareNow();
    const qint64 a = awakeNow();
    if (m_lastProbeMs >= 0) {
        // GUI-thread lateness: the lag gate only.
        const qint64 lateness = (t - m_lastProbeMs) - kProbeIntervalMs;
        m_worstLatenessMs = std::max(m_worstLatenessMs, lateness);
        if (lateness > kLagThresholdMs)
            m_lateAt.append(t);
        // Time that passed for the machine but not for the awake clock: the
        // process was suspended. A stall moves both clocks alike.
        const qint64 suspended = m_lastProbeSuspendAwareMs >= 0 && m_lastProbeAwakeMs >= 0
            ? (s - m_lastProbeSuspendAwareMs) - (a - m_lastProbeAwakeMs)
            : 0;
        if (suspended > kResumeLagMs) {
            // Every catch-up after a suspend waits for the foreground first.
            m_foregroundFirstUntilMs = t + kResumeForegroundGraceMs;
            qCInfo(lcScheduler) << "resume from suspend detected"
                                << "suspended_ms=" << suspended;
            Q_EMIT resumeDetected();
        }
    }
    m_lastProbeMs = t;
    m_lastProbeSuspendAwareMs = s;
    m_lastProbeAwakeMs = a;
    const bool wasClear = m_lateAt.isEmpty();
    while (!m_lateAt.isEmpty() && m_lateAt.first() <= t - kLagWindowMs)
        m_lateAt.removeFirst();
    if (!wasClear && m_lateAt.isEmpty())
        Q_EMIT admissionMayHaveChanged();
}

bool SessionScheduler::lagClear() const
{
    const qint64 t = now();
    for (qint64 at : m_lateAt) {
        if (at > t - kLagWindowMs)
            return false;
    }
    return true;
}

void SessionScheduler::noteStarted()
{
    m_startedMs = now();
}

void SessionScheduler::noteForegroundInitialSyncDone()
{
    m_foregroundSynced = true;
    // The foreground caught up: a resume's "foreground first" is over.
    m_foregroundFirstUntilMs = 0;
    Q_EMIT admissionMayHaveChanged();
}

bool SessionScheduler::gateOpen() const
{
    if (m_startedMs < 0)
        return false;
    const bool foregroundReady =
        m_foregroundSynced || now() - m_startedMs >= kGateFallbackMs;
    return foregroundReady && lagClear();
}

bool SessionScheduler::catchUpFree() const
{
    const qint64 t = now();
    return m_catchUpHolder.isEmpty() && t >= m_catchUpCooldownUntilMs
        && t >= m_foregroundFirstUntilMs;
}

void SessionScheduler::acquireCatchUp(const QString &userId)
{
    m_catchUpHolder = userId;
}

void SessionScheduler::releaseCatchUp(const QString &userId)
{
    if (m_catchUpHolder != userId)
        return;
    m_catchUpHolder.clear();
    m_catchUpCooldownUntilMs = now() + qint64(unit() * 1000.0);
    Q_EMIT admissionMayHaveChanged();
}

bool SessionScheduler::homeserverStarting(const QString &homeserver) const
{
    return m_startingByHomeserver.contains(homeserver);
}

void SessionScheduler::noteStarting(const QString &homeserver, const QString &userId)
{
    m_startingByHomeserver.insert(homeserver, userId);
}

void SessionScheduler::noteStartFinished(const QString &homeserver, const QString &userId)
{
    const auto it = m_startingByHomeserver.constFind(homeserver);
    if (it != m_startingByHomeserver.cend() && it.value() == userId) {
        m_startingByHomeserver.remove(homeserver);
        Q_EMIT admissionMayHaveChanged();
    }
}

bool SessionScheduler::homeserverPaused(const QString &homeserver) const
{
    const auto it = m_pausedUntil.constFind(homeserver);
    return it != m_pausedUntil.cend() && now() < it.value();
}

void SessionScheduler::pauseHomeserver(const QString &homeserver, qint64 forMs)
{
    if (homeserver.isEmpty())
        return;
    m_pausedUntil.insert(homeserver, now() + forMs);
}

void SessionScheduler::noteHomeserverReachable(const QString &homeserver)
{
    if (homeserver.isEmpty())
        return;
    bool changed = m_pausedUntil.remove(homeserver) > 0;
    for (auto it = m_backoff.begin(); it != m_backoff.end(); ++it) {
        if (it->homeserver == homeserver && !it->permanent && it->untilMs > 0) {
            it->untilMs = 0;
            it->failures = 0;
            changed = true;
        }
    }
    if (changed)
        Q_EMIT admissionMayHaveChanged();
}

qint64 SessionScheduler::backoffDelayMs(int failures, double unit)
{
    static constexpr qint64 kMinutes[] = {1, 5, 15, 60};
    const int index = std::clamp(failures - 1, 0, 3);
    const double factor = 0.8 + 0.4 * std::clamp(unit, 0.0, 1.0);
    return qint64(double(kMinutes[index] * 60 * 1000) * factor);
}

void SessionScheduler::noteFailure(const QString &userId, const QString &homeserver,
                                   bool permanent)
{
    Backoff &b = m_backoff[userId];
    b.homeserver = homeserver;
    if (permanent) {
        b.permanent = true;
        return;
    }
    ++b.failures;
    b.untilMs = now() + backoffDelayMs(b.failures, unit());
}

void SessionScheduler::noteSuccess(const QString &userId)
{
    auto it = m_backoff.find(userId);
    if (it != m_backoff.end() && !it->permanent)
        m_backoff.erase(it);
}

bool SessionScheduler::inBackoff(const QString &userId) const
{
    const auto it = m_backoff.constFind(userId);
    return it != m_backoff.cend() && !it->permanent && now() < it->untilMs;
}

bool SessionScheduler::permanentlyRefused(const QString &userId) const
{
    const auto it = m_backoff.constFind(userId);
    return it != m_backoff.cend() && it->permanent;
}

int SessionScheduler::failures(const QString &userId) const
{
    return m_backoff.value(userId).failures;
}

void SessionScheduler::clearFailures(const QString &userId)
{
    m_backoff.remove(userId);
}

bool SessionScheduler::underCap(int liveHosts, bool foregroundLive) const
{
    return liveHosts + (foregroundLive ? 1 : 0) < m_cap;
}

qint64 SessionScheduler::fdSoftLimit() const
{
    if (m_fdLimitOverride >= 0)
        return m_fdLimitOverride;
#ifdef Q_OS_UNIX
    struct rlimit limit {};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
        return qint64(limit.rlim_cur);
#endif
    return 0;
}

bool SessionScheduler::fdHeadroomFor(int hostsAfterStart) const
{
    const qint64 soft = fdSoftLimit();
    if (soft <= 0)
        return true;
    // Configured maxima, not observed use (HYPOTHESIS until S5): every host
    // and the foreground at about 56, on top of the application's own.
    const qint64 needed = kFdBaseline + qint64(kFdsPerHost) * (hostsAfterStart + 1);
    return double(needed) <= double(soft) * (1.0 - kFdSpare);
}

bool SessionScheduler::memoryGuardWouldPause()
{
#ifdef Q_OS_LINUX
    qint64 rssBytes = 0;
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (statm.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
        if (fields.size() >= 2)
            rssBytes = fields.at(1).toLongLong() * qint64(sysconf(_SC_PAGESIZE));
    }
    qint64 totalKb = 0;
    qint64 availableKb = 0;
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : meminfo.readAll().split('\n')) {
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() < 2)
                continue;
            if (parts.at(0) == "MemTotal:")
                totalKb = parts.at(1).toLongLong();
            else if (parts.at(0) == "MemAvailable:")
                availableKb = parts.at(1).toLongLong();
        }
    }
    if (rssBytes <= 0 || totalKb <= 0)
        return false;
    const qint64 rssCeiling = std::max<qint64>(qint64(1536) * 1024 * 1024, totalKb * 1024 / 4);
    const bool over = rssBytes > rssCeiling || availableKb * 10 < totalKb;
    if (over != m_memoryReported) {
        m_memoryReported = over;
        qCInfo(lcScheduler) << "memory guard (report-only)"
                            << "would_pause=" << over
                            << "rss_mb=" << rssBytes / (1024 * 1024)
                            << "available_pct=" << (availableKb * 100 / totalKb);
    }
    return over;
#else
    return false;
#endif
}

} // namespace lightning::accounts
