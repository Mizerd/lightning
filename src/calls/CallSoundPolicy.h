// Which call sound plays, and when. Policy only: no audio, no QObject.
// CallSoundController feeds it state and passes results to a sink; tests
// drive it directly.
//
//   * Every cue is local; the far end never hears them. While a screen share
//     captures this computer's whole output mix (e.g. Windows loopback without
//     process exclusion), every cue is suppressed (setOutputCapturedByShare).
//   * No chorus at join: after every connect the roster is re-baselined
//     silently for kSettleMs, and join/leave are throttled to one per
//     kPresenceThrottleMs (Element Call also uses 500 ms).
//   * Big calls are quiet: join/leave stop above kMaxCallSizeForPresence
//     people (Element Call: 8).
//   * Deafen silences other people's cues (join, leave, share, hand), not the
//     user's own actions, and not an incoming ring.
//   * A call that never got going ends silently (failed join, declined ring),
//     as in Element Web.
//   * No cue for your own raised hand or camera.
//   * A chat notification's chime is not a call cue, but it asks this policy
//     whether the call state allows it (allowsNotificationSound).
#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace callsound {

enum class Cue {
    Join,
    Leave,
    Connected,
    Ended,
    Disconnected,
    Mute,
    Unmute,
    Deafen,
    Undeafen,
    ShareStart,
    ShareStop,
    HandRaised,
};

enum class Loop {
    None,
    Ring,
    CallWaiting,
    Ringback,
};

enum class Category {
    Presence,
    Controls,
    ShareAndHand,
};

Category categoryOf(Cue cue);
QString soundName(Cue cue);
QString soundName(Loop loop);

struct Preferences {
    bool enabled = true;
    bool presence = true;
    bool controls = true;
    bool shareAndHand = true;
};

struct RemoteParticipant {
    bool handRaised = false;
    bool sharing = false;
};
using Roster = QHash<QString, RemoteParticipant>;

enum class GroupPhase { Idle, Joining, Connected, Reconnecting, Ended };
enum class LegacyPhase {
    Idle,
    OutgoingRinging,
    IncomingRinging,
    Connecting,
    Active,
    Ended,
};
enum class Lane { Group, Legacy };

class Policy
{
public:
    static constexpr qint64 kSettleMs = 2000;
    static constexpr qint64 kPresenceThrottleMs = 500;
    static constexpr int kMaxCallSizeForPresence = 8;

    void setPreferences(const Preferences &prefs) { m_prefs = prefs; }
    const Preferences &preferences() const { return m_prefs; }

    /// True while a screen share captures this computer's whole output mix,
    /// so anything played here would reach the call.
    void setOutputCapturedByShare(bool captured) { m_outputCaptured = captured; }

    QList<Cue> groupPhaseChanged(GroupPhase phase, qint64 nowMs);
    QList<Cue> rosterChanged(const Roster &remotes, qint64 nowMs);
    QList<Cue> legacyPhaseChanged(LegacyPhase phase, bool remoteEndedOutgoing);
    QList<Cue> localAudioChanged(Lane lane, bool micMuted, bool deafened);
    QList<Cue> localShareChanged(bool sharing);

    void setIncomingRing(bool ringing) { m_incomingRing = ringing; }
    bool incomingRing() const { return m_incomingRing; }
    Loop desiredLoop() const;

    bool inCall() const;
    bool deafened() const;
    bool allowsNotificationSound() const;
    GroupPhase groupPhase() const { return m_group; }
    LegacyPhase legacyPhase() const { return m_legacy; }

private:
    struct AudioState {
        bool known = false;
        bool muted = false;
        bool deafened = false;
    };

    bool groupLive() const;
    bool legacyLive() const;
    bool allowed(Cue cue, bool remote) const;
    bool throttled(Cue cue, qint64 nowMs);
    QList<Cue> keepAllowed(const QList<Cue> &cues, bool remote) const;

    Preferences m_prefs;
    bool m_outputCaptured = false;
    bool m_incomingRing = false;
    GroupPhase m_group = GroupPhase::Idle;
    LegacyPhase m_legacy = LegacyPhase::Idle;
    qint64 m_connectedAtMs = 0;
    Roster m_roster;
    AudioState m_groupAudio;
    AudioState m_legacyAudio;
    bool m_localShareKnown = false;
    bool m_localSharing = false;
    QHash<int, qint64> m_lastPlayedMs;
};

}
