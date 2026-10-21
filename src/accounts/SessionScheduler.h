// When a background host may start or catch up (live accounts, Labs; design
// §6.1). Policy and bookkeeping only: it starts nothing itself; SessionPool
// asks it and acts. GUI thread only.
//
//  * Lag probe: a 100 ms QTimer measured with a monotonic clock, always on
//    while Labs is on. "Clear" means no lateness over 50 ms in the last 2 s.
//    It does not depend on the opt-in stall tracer.
//  * Resume: a SECOND clock that keeps counting while the machine sleeps
//    (CLOCK_BOOTTIME on Linux, CLOCK_MONOTONIC on Apple, GetTickCount64 on
//    Windows, the wall clock elsewhere) against an AWAKE clock that does not
//    (the monotonic clock; QueryUnbiasedInterruptTime on Windows, because
//    QElapsedTimer's QueryPerformanceCounter may count sleep). Between two
//    ticks, the first gap minus the second is the time spent suspended; over
//    10 s is a resume. Lateness alone cannot see a suspend, and a GUI stall
//    moves both clocks alike, so it is never read as one (review M3, round
//    2). NOT measured on a real suspend on any platform. Where the wall clock
//    is the fallback, a clock set forward by over 10 s also reads as a
//    resume (every host merely catches up).
//  * Gate: nothing starts before the foreground's initial sync (or 15 s) and
//    a clear probe.
//  * Catch-up semaphore: one host catching up at a time, process-wide, for
//    every catch-up (first start, reconnect, resume). Released on catch-up
//    complete (CatchUpTracker), then U(0, 1 s). After a resume the
//    foreground goes first.
//  * At most one Starting host per homeserver; a 429 pauses a homeserver
//    for 5 min; a host's failure backoff is 1, 5, 15, 60 min ±20 %, and is
//    reset when the foreground reports that homeserver reachable again.
//    Permanent refusals get no retry until the account is pinned again.
//  * Admission: the cap, fd headroom from configured maxima (about 56 per
//    host against the soft limit with 30 % spare), and a memory guard that
//    only REPORTS until the S5 scale run sets its thresholds (DECISION 9).
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

namespace lightning::accounts {

// Is a host's catch-up complete? The first room_list_sync_state "running"
// after initial_sync_done, seen after the catch-up began, with the host's
// notable-update rate under 5/s over 3 s; or 60 s, whichever is first.
// Pure, so the rule is tested without a host. HYPOTHESIS: whether these
// signals track real catch-up I/O is measured in S5/S6b.
class CatchUpTracker
{
public:
    static constexpr qint64 kRateWindowMs = 3000;
    static constexpr double kMaxNotablePerSecond = 5.0;
    static constexpr qint64 kCapMs = 60000;

    void begin(qint64 nowMs, quint64 sessionStateVersion);
    void sample(qint64 nowMs, quint64 notableUpdatesTotal);
    bool active() const { return m_startedMs >= 0; }
    bool complete(qint64 nowMs, bool initialSyncDone, bool roomListRunning,
                  quint64 sessionStateVersion) const;
    void reset();

private:
    qint64 m_startedMs = -1;
    quint64 m_baseVersion = 0;
    QList<QPair<qint64, quint64>> m_samples;   // (time, notable total), oldest first
};

class SessionScheduler : public QObject
{
    Q_OBJECT
public:
    static constexpr int kProbeIntervalMs = 100;
    static constexpr qint64 kLagThresholdMs = 50;
    static constexpr qint64 kLagWindowMs = 2000;
    static constexpr qint64 kResumeLagMs = 10000;
    static constexpr qint64 kGateFallbackMs = 15000;
    static constexpr qint64 kResumeForegroundGraceMs = 15000;
    static constexpr qint64 kHomeserverPauseMs = 5 * 60 * 1000;
    static constexpr int kFdsPerHost = 56;
    static constexpr int kFdBaseline = 256;
    static constexpr double kFdSpare = 0.30;

    // monotonicMs: nullptr = a QElapsedTimer started here. jitterUnit: a
    // uniform [0, 1) source; nullptr = QRandomGenerator.
    explicit SessionScheduler(std::function<qint64()> monotonicMs = {},
                              std::function<double()> jitterUnit = {},
                              QObject *parent = nullptr);

    qint64 now() const;
    // The clock that counts suspended time (above); nullptr = the platform's.
    void setSuspendAwareClock(std::function<qint64()> suspendAwareMs);
    qint64 suspendAwareNow() const;
    qint64 awakeNow() const;          // the clock suspendAwareNow() is measured against
    double unit() const;

    // ---- lag probe ----------------------------------------------------------
    void startProbe();
    void stopProbe();
    bool probeRunning() const { return m_probe.isActive(); }
    // What the probe's timer runs; a test calls it with a scripted clock.
    void probeTick();
    bool lagClear() const;
    qint64 worstLatenessMs() const { return m_worstLatenessMs; }

    // ---- gate ---------------------------------------------------------------
    void noteStarted();
    void noteForegroundInitialSyncDone();
    bool gateOpen() const;

    // ---- catch-up semaphore ---------------------------------------------------
    bool catchUpFree() const;
    void acquireCatchUp(const QString &userId);
    // Releases only when `userId` holds it; then U(0, 1 s) before the next.
    void releaseCatchUp(const QString &userId);
    QString catchUpHolder() const { return m_catchUpHolder; }

    // ---- homeservers ------------------------------------------------------------
    bool homeserverStarting(const QString &homeserver) const;
    void noteStarting(const QString &homeserver, const QString &userId);
    void noteStartFinished(const QString &homeserver, const QString &userId);
    bool homeserverPaused(const QString &homeserver) const;
    void pauseHomeserver(const QString &homeserver, qint64 forMs = kHomeserverPauseMs);
    // The foreground reached it: its pause ends and its hosts' backoff resets.
    void noteHomeserverReachable(const QString &homeserver);

    // ---- failures ---------------------------------------------------------------
    // 1, 5, 15, 60 min by failure count, x (0.8 + 0.4 * unit).
    static qint64 backoffDelayMs(int failures, double unit);
    void noteFailure(const QString &userId, const QString &homeserver, bool permanent);
    void noteSuccess(const QString &userId);
    bool inBackoff(const QString &userId) const;
    bool permanentlyRefused(const QString &userId) const;
    int failures(const QString &userId) const;
    // A pin turned on again: the user asked to try once more.
    void clearFailures(const QString &userId);

    // ---- admission ----------------------------------------------------------------
    void setCap(int maxLive) { m_cap = maxLive; }
    // hosts + the foreground (when signed in) stay under the cap.
    bool underCap(int liveHosts, bool foregroundLive) const;
    // Soft RLIMIT_NOFILE (Unix); 0 = unlimited / not applicable.
    qint64 fdSoftLimit() const;
    void setFdSoftLimitForTest(qint64 limit) { m_fdLimitOverride = limit; }
    bool fdHeadroomFor(int hostsAfterStart) const;
    // Report-only (DECISION 9): logs when RSS or MemAvailable cross the
    // design's thresholds; never refuses. Returns what it would have said.
    bool memoryGuardWouldPause();

Q_SIGNALS:
    // Over 10 s spent suspended between two ticks. Every running host
    // catches up again, one at a time, after the foreground.
    void resumeDetected();
    // Something a waiting start depends on changed (gate, semaphore, pause).
    void admissionMayHaveChanged();

private:
    std::function<qint64()> m_monotonicMs;
    std::function<double()> m_jitterUnit;
    std::function<qint64()> m_suspendAwareMs;
    QElapsedTimer m_clock;
    QTimer m_probe;
    qint64 m_lastProbeMs = -1;
    qint64 m_lastProbeSuspendAwareMs = -1;
    qint64 m_lastProbeAwakeMs = -1;
    qint64 m_worstLatenessMs = 0;
    QList<qint64> m_lateAt;           // times of a lateness over the threshold, oldest first
    qint64 m_startedMs = -1;
    bool m_foregroundSynced = false;
    QString m_catchUpHolder;
    qint64 m_catchUpCooldownUntilMs = 0;
    qint64 m_foregroundFirstUntilMs = 0;
    QHash<QString, QString> m_startingByHomeserver;
    QHash<QString, qint64> m_pausedUntil;
    struct Backoff {
        int failures = 0;
        qint64 untilMs = 0;
        bool permanent = false;
        QString homeserver;
    };
    QHash<QString, Backoff> m_backoff;
    int m_cap = 5;
    qint64 m_fdLimitOverride = -1;
    bool m_memoryReported = false;
};

} // namespace lightning::accounts
