// Which call sound plays, and when. POLICY ONLY: no audio, no QObject, no
// Qt Multimedia — CallSoundController feeds it the call lanes' state and
// hands what it returns to a sink, and the tests drive it directly.
//
// What it decides, and why each rule exists:
//
//   * EVERY CUE IS LOCAL. Nothing here is mixed into the call; the far end
//     never hears our join chime. The one way a cue COULD reach them is a
//     screen share carrying this computer's output mix (Windows loopback
//     without the per-process exclusion), and while that is live every cue
//     is suppressed — `setOutputCapturedByShare`.
//   * NO CHORUS AT JOIN. The participants already in a call arrive as one
//     roster update when we connect, and again after a reconnect; announcing
//     them would play a join per person. The roster is re-baselined SILENTLY
//     for `kSettleMs` after every connect, and join/leave are throttled to
//     one per `kPresenceThrottleMs` (Element Call uses the same 500 ms).
//   * BIG CALLS ARE QUIET. Join/leave stop once the call holds more than
//     `kMaxCallSizeForPresence` people (Element Call: 8; Google Meet stops
//     after the first five joiners). In a large call they are noise.
//   * DEAFEN SILENCES THE ROOM, NOT YOUR HANDS. Deafened, nothing other
//     people do makes a sound — no join, leave, share or hand — because
//     deafen is the user asking to hear nothing from the call. Discord keeps
//     playing join/leave while deafened and its users have asked for years
//     for a way to stop that. Your OWN actions still confirm (mute, undeafen,
//     leaving, your share), and so does an incoming ring: a new call is not
//     this call's audio, and missing it is worse than hearing it.
//   * A CALL THAT NEVER GOT OFF THE GROUND ENDS SILENTLY. No "call ended"
//     for a join that failed before connecting or a ring you declined —
//     Element Web's rule, and the right one: nothing had started.
//   * NO SOUND FOR YOUR OWN RAISED HAND OR YOUR CAMERA. A raise is addressed
//     to others and the button already shows it; no surveyed client plays a
//     camera cue.
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
/// The sound file's base name (data/sounds/<name>.wav). Also the log name.
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

    /// True while a screen share is capturing this computer's whole output
    /// mix, so anything played here would be heard by the call.
    void setOutputCapturedByShare(bool captured) { m_outputCaptured = captured; }

    QList<Cue> groupPhaseChanged(GroupPhase phase, qint64 nowMs);
    /// The complete set of REMOTE participants as it stands now.
    QList<Cue> rosterChanged(const Roster &remotes, qint64 nowMs);
    /// `remoteEndedOutgoing`: an outgoing call ended by the other side
    /// (declined, busy, never answered) rather than by us.
    QList<Cue> legacyPhaseChanged(LegacyPhase phase, bool remoteEndedOutgoing);
    QList<Cue> localAudioChanged(Lane lane, bool micMuted, bool deafened);
    QList<Cue> localShareChanged(bool sharing);

    /// Whether an ANNOUNCED incoming ring is live (the ring's own gates —
    /// notifications, ignored senders, muted rooms, the ring switch — are
    /// applied by whoever announces it, not here).
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
