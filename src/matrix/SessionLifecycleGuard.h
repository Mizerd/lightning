#pragma once

#include <QtGlobal>

// Small, testable lifecycle gate for asynchronous backend callbacks. A Rust
// handle is stamped with the generation that created it. Sign-out invalidates
// that generation immediately, before the old sync task is stopped, so queued
// room/timeline/error callbacks can no longer mutate visible state.
class SessionLifecycleGuard
{
public:
    quint64 beginSession()
    {
        m_signingOut = false;
        m_shutdownSource = 0;
        return ++m_active;
    }

    quint64 beginSignOut(quint64 handleGeneration)
    {
        m_signingOut = true;
        m_shutdownSource = handleGeneration;
        ++m_active;
        return m_active;
    }

    quint64 invalidate()
    {
        m_signingOut = false;
        m_shutdownSource = 0;
        return ++m_active;
    }

    bool acceptsActive(quint64 callbackGeneration) const
    {
        return !m_signingOut && callbackGeneration != 0
            && callbackGeneration == m_active;
    }

    bool acceptsShutdownCompletion(quint64 callbackGeneration) const
    {
        return m_signingOut && callbackGeneration != 0
            && callbackGeneration == m_shutdownSource;
    }

    void finishSignOut()
    {
        m_signingOut = false;
        m_shutdownSource = 0;
    }

    quint64 activeGeneration() const { return m_active; }
    quint64 shutdownSourceGeneration() const { return m_shutdownSource; }
    bool signingOut() const { return m_signingOut; }

private:
    quint64 m_active = 0;
    quint64 m_shutdownSource = 0;
    bool m_signingOut = false;
};
