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
#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace callsound {

enum class Cue {
    Join,         // someone else joined
    Leave,        // someone else left
    Connected,    // you joined (or got back after a reconnect)
    Ended,        // you left, or the call ended under you
    Disconnected, // the connection dropped and is being retried
    Mute,
    Unmute,
    Deafen,
    Undeafen,
    ShareStart, // anyone's screen share went live
    ShareStop,  // anyone's screen share ended
    HandRaised, // someone ELSE raised a hand
};

enum class Loop {
    None,
    Ring,        // an incoming call, nothing else going on
    CallWaiting, // an incoming call while you are already in one
    Ringback,    // your outgoing 1:1 call is ringing at the other end
};

enum class Category {
    Presence,     // join, leave, connected, ended, disconnected
    Controls,     // mute, unmute, deafen, undeafen
    ShareAndHand, // screen share start/stop, raised hand
};

Category categoryOf(Cue cue);
/// The sound file's base name (data/sounds/<name>.wav), also used in logs.
QString soundName(Cue cue);
QString soundName(Loop loop);

struct Preferences {
    bool enabled = true; // every in-call cue, and the ringback
    bool presence = true;
    bool controls = true;
    bool shareAndHand = true;
};

/// One REMOTE participant, keyed by LiveKit identity (one per device).
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
    /// The complete set of REMOTE participants as it stands now.
    QList<Cue> rosterChanged(const Roster &remotes, qint64 nowMs);
    /// `remoteEndedOutgoing`: an outgoing call ended by the other side
    /// (declined, busy, never answered) rather than by us.
    QList<Cue> legacyPhaseChanged(LegacyPhase phase, bool remoteEndedOutgoing);
    QList<Cue> localAudioChanged(Lane lane, bool micMuted, bool deafened);
    QList<Cue> localShareChanged(bool sharing);

    /// Whether an announced incoming ring is live; its gates (notifications,
    /// ignored senders, muted rooms, the ring switch) belong to the announcer.
    void setIncomingRing(bool ringing) { m_incomingRing = ringing; }
    bool incomingRing() const { return m_incomingRing; }
    Loop desiredLoop() const;

    bool inCall() const;
    bool deafened() const;
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
    QHash<int, qint64> m_lastPlayedMs; // Cue -> when it last played
};

} // namespace callsound
