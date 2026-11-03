#pragma once

#include <atomic>

/// The one in-place restart a screen share's capture may take. Messages are
/// stamped with stamp() on whatever thread posts them; the GUI thread asks
/// isStale() of that stamp.
class ShareCaptureRetry
{
public:
    /// GUI thread. False once a restart has been asked for.
    bool begin()
    {
        int expected = 0;
        return m_requested.compare_exchange_strong(expected, 1);
    }
    int requested() const { return m_requested.load(); }
    bool isStale(int stamp) const { return stamp >= 0 && stamp < requested(); }

    int stamp() const { return m_published.load(); }

    static bool mayRestart(bool binTargetPlaying, bool binHasParent)
    {
        return binTargetPlaying && binHasParent;
    }
    /// Restart thread, only after the superseded attempt's streaming thread
    /// has been joined: anything it posted still carries the old stamp.
    void publish() { m_published.store(m_requested.load()); }

    /// Streaming thread. True exactly once: the first buffer of a restart.
    bool noteCaptured()
    {
        return m_published.load() > 0 && !m_confirmed.exchange(true);
    }

private:
    std::atomic<int> m_requested{0};
    std::atomic<int> m_published{0};
    std::atomic<bool> m_confirmed{false};
};
