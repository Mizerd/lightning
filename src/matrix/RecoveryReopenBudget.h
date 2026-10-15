#pragma once

#include <QHash>
#include <QString>

#include <deque>

namespace matrix {

// A loop breaker for the reopens that recover a damaged timeline mirror
// (`DiffOutcome::Invalid`, `queue_overflow`).
//
// Those callers reopen the SAME timeline on purpose: the mirror is known to be
// wrong, so an identity guard ("already open, skip") would turn the recovery
// into a no-op and leave it wrong for good. What they lacked is a bound. A
// diff that is invalid again after every reopen (a Set the reopened snapshot
// still cannot hold, a retry_decryption storm) reopened without limit and
// without a word. Now at most kMaxReopens run per kWindowMs per timeline; past
// that the caller logs one escalation and schedules ONE deferred reopen for
// when the window has room again, so the mirror still recovers, later, and the
// next capture has a named line to grep for.
//
// Pure: the caller passes a monotonic clock in milliseconds and owns the
// timer and the log line.
class RecoveryReopenBudget
{
public:
    static constexpr int kMaxReopens = 3;
    static constexpr qint64 kWindowMs = 60 * 1000;

    enum class Verdict {
        // Reopen now; it has been counted.
        Reopen,
        // Over the cap. Log the escalation and schedule one deferred reopen
        // after `retryInMs`, then call takeDeferred() when it fires.
        Defer,
        // Over the cap with a deferred reopen already scheduled: do nothing.
        AlreadyDeferred,
    };

    Verdict request(const QString &timelineId, qint64 nowMs, qint64 *retryInMs)
    {
        Entry &entry = m_entries[timelineId];
        prune(entry, nowMs);
        if (entry.deferred)
            return Verdict::AlreadyDeferred;
        if (static_cast<int>(entry.reopens.size()) < kMaxReopens) {
            entry.reopens.push_back(nowMs);
            return Verdict::Reopen;
        }
        entry.deferred = true;
        if (retryInMs)
            *retryInMs = qMax<qint64>(0, entry.reopens.front() + m_windowMs - nowMs);
        return Verdict::Defer;
    }

    // The deferred reopen fired. True when it is still owed (it is counted
    // now); false when clear() dropped it in the meantime.
    bool takeDeferred(const QString &timelineId, qint64 nowMs)
    {
        auto it = m_entries.find(timelineId);
        if (it == m_entries.end() || !it->deferred)
            return false;
        prune(*it, nowMs);
        it->deferred = false;
        it->reopens.push_back(nowMs);
        return true;
    }

    // Session end: nothing owed to a timeline of the previous session.
    void clear() { m_entries.clear(); }

    qint64 windowMs() const { return m_windowMs; }
    // Tests only: a short window so a deferred reopen fires within a test.
    void setWindowMsForTest(qint64 ms) { m_windowMs = ms; }

    int reopensInWindow(const QString &timelineId, qint64 nowMs)
    {
        auto it = m_entries.find(timelineId);
        if (it == m_entries.end())
            return 0;
        prune(*it, nowMs);
        return static_cast<int>(it->reopens.size());
    }

private:
    struct Entry {
        std::deque<qint64> reopens;
        bool deferred = false;
    };

    void prune(Entry &entry, qint64 nowMs) const
    {
        while (!entry.reopens.empty() && nowMs - entry.reopens.front() >= m_windowMs)
            entry.reopens.pop_front();
    }

    QHash<QString, Entry> m_entries;
    qint64 m_windowMs = kWindowMs;
};

} // namespace matrix
