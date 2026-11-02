#pragma once

#include <QString>

#include <optional>

namespace lightning::calls {

class LegacyFallbackArm
{
public:
    enum class Phase {
        Idle,
        Preparing,
        PastTheGate,
        Failed,
        Ended,
    };

    void buttonJoinDispatched(const QString &roomId, bool video, bool ok)
    {
        m_roomId = ok ? roomId : QString();
        m_video = ok && video;
    }

    void phaseChanged(Phase phase)
    {
        if (phase != Phase::Failed)
            clear();
    }

    void roomChanged() { clear(); }

    bool armedFor(const QString &roomId) const
    {
        return !roomId.isEmpty() && roomId == m_roomId;
    }

    struct Take {
        bool fallBack = false;
        bool video = false;
    };
    Take take(const QString &roomId)
    {
        Take out;
        if (armedFor(roomId)) {
            out.fallBack = true;
            out.video = m_video;
        }
        clear();
        return out;
    }

private:
    void clear()
    {
        m_roomId.clear();
        m_video = false;
    }

    QString m_roomId;
    bool m_video = false;
};

class CallFailureNotice
{
public:
    void reported(const QString &message) { m_shown = message; }

    std::optional<QString> failed(const QString &reason)
    {
        // An empty reason is a withdrawal, and it must not clear a message
        // that is not ours.
        if (reason.isEmpty())
            return withdraw();
        m_ours = reason;
        return reason;
    }

    std::optional<QString> withdraw()
    {
        if (m_ours.isEmpty())
            return std::nullopt;
        const bool stillShown = m_shown == m_ours;
        m_ours.clear();
        if (!stillShown)
            return std::nullopt;
        return QString();
    }

    std::optional<QString> roomChanged(bool callRunning)
    {
        if (callRunning)
            return std::nullopt;
        return withdraw();
    }

private:
    QString m_shown;
    QString m_ours;
};

}
