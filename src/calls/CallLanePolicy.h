// Two small rules AppController applies between the call lanes, kept here so
// they can be tested without a media engine or a MatrixRTC service.
//
// LegacyFallbackArm: when the homeserver refuses our MatrixRTC membership as
// forbidden, a join the CALL BUTTON started in a 1:1 DM is re-placed on the
// legacy lane. A join from the call banner, the timeline's call row or the
// incoming-call card asks for that particular call and must never turn into a
// legacy ring, so the arm is dropped as soon as any other join starts.
//
// CallFailureNotice: the status strip shows the last errorReported(), shared
// by everything in the app. A call failure is withdrawn only while it is
// still what the strip shows, never someone else's message, and not by room
// navigation while a call is running (its notices are still true then).
#pragma once

#include <QString>

#include <optional>

namespace lightning::calls {

class LegacyFallbackArm
{
public:
    /// What the group call is doing, as far as the fallback cares.
    enum class Phase {
        Idle,
        /// Publishing the membership: some join started.
        Preparing,
        /// The membership was accepted (Authorizing or later).
        PastTheGate,
        /// Torn down with a failure. Kept: a forbidden refusal is reported
        /// right after this state.
        Failed,
        Ended,
    };

    /// The call button's MatrixRTC join returned. Called after join(), whose
    /// own Preparing already cleared any older arm.
    void buttonJoinDispatched(const QString &roomId, bool video, bool ok)
    {
        m_roomId = ok ? roomId : QString();
        m_video = ok && video;
    }

    void phaseChanged(Phase phase)
    {
        // Any new join (a banner or card join included) supersedes the arm;
        // the button re-arms right after its own join returns.
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
    /// The server refused the membership in `roomId` as forbidden. Consumes
    /// the arm either way.
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
    /// Every errorReported(), whoever sent it: what the strip shows now.
    void reported(const QString &message) { m_shown = message; }

    /// A call lane reported a failure. Returns what to report: the reason,
    /// the empty withdrawal when our notice is still shown, or nothing.
    std::optional<QString> failed(const QString &reason)
    {
        // An empty reason is a withdrawal, and it must not clear a message
        // that is not ours.
        if (reason.isEmpty())
            return withdraw();
        m_ours = reason;
        return reason;
    }

    /// Our notice no longer applies (a call carried on another lane).
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

    /// The user opened another room. A notice about a call that is still
    /// running ("call audio stopped", "nobody hears you") stays.
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

} // namespace lightning::calls
