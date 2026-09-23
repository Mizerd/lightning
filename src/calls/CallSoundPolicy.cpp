#include "calls/CallSoundPolicy.h"

namespace callsound {

Category categoryOf(Cue cue)
{
    switch (cue) {
    case Cue::Join:
    case Cue::Leave:
    case Cue::Connected:
    case Cue::Ended:
    case Cue::Disconnected:
        return Category::Presence;
    case Cue::Mute:
    case Cue::Unmute:
    case Cue::Deafen:
    case Cue::Undeafen:
        return Category::Controls;
    case Cue::ShareStart:
    case Cue::ShareStop:
    case Cue::HandRaised:
        return Category::ShareAndHand;
    }
    return Category::Presence;
}

QString soundName(Cue cue)
{
    switch (cue) {
    case Cue::Join: return QStringLiteral("join");
    case Cue::Leave: return QStringLiteral("leave");
    case Cue::Connected: return QStringLiteral("connected");
    case Cue::Ended: return QStringLiteral("ended");
    case Cue::Disconnected: return QStringLiteral("disconnected");
    case Cue::Mute: return QStringLiteral("mute");
    case Cue::Unmute: return QStringLiteral("unmute");
    case Cue::Deafen: return QStringLiteral("deafen");
    case Cue::Undeafen: return QStringLiteral("undeafen");
    case Cue::ShareStart: return QStringLiteral("share-start");
    case Cue::ShareStop: return QStringLiteral("share-stop");
    case Cue::HandRaised: return QStringLiteral("hand-raised");
    }
    return QString();
}

QString soundName(Loop loop)
{
    switch (loop) {
    case Loop::None: return QString();
    case Loop::Ring: return QStringLiteral("ring");
    case Loop::CallWaiting: return QStringLiteral("call-waiting");
    case Loop::Ringback: return QStringLiteral("ringback");
    }
    return QString();
}

bool Policy::groupLive() const
{
    return m_group == GroupPhase::Joining || m_group == GroupPhase::Connected
        || m_group == GroupPhase::Reconnecting;
}

bool Policy::legacyLive() const
{
    return m_legacy == LegacyPhase::OutgoingRinging
        || m_legacy == LegacyPhase::Connecting
        || m_legacy == LegacyPhase::Active;
}

bool Policy::inCall() const
{
    return groupLive() || m_legacy == LegacyPhase::Connecting
        || m_legacy == LegacyPhase::Active;
}

bool Policy::deafened() const
{
    return (groupLive() && m_groupAudio.deafened)
        || (legacyLive() && m_legacyAudio.deafened);
}

bool Policy::allowed(Cue cue, bool remote) const
{
    if (!m_prefs.enabled || m_outputCaptured)
        return false;
    if (remote && deafened())
        return false;
    switch (categoryOf(cue)) {
    case Category::Presence: return m_prefs.presence;
    case Category::Controls: return m_prefs.controls;
    case Category::ShareAndHand: return m_prefs.shareAndHand;
    }
    return false;
}

QList<Cue> Policy::keepAllowed(const QList<Cue> &cues, bool remote) const
{
    QList<Cue> out;
    for (Cue cue : cues) {
        if (allowed(cue, remote))
            out.append(cue);
    }
    return out;
}

bool Policy::throttled(Cue cue, qint64 nowMs)
{
    const auto it = m_lastPlayedMs.constFind(int(cue));
    if (it != m_lastPlayedMs.constEnd() && nowMs - *it < kPresenceThrottleMs)
        return true;
    m_lastPlayedMs.insert(int(cue), nowMs);
    return false;
}

QList<Cue> Policy::groupPhaseChanged(GroupPhase phase, qint64 nowMs)
{
    const GroupPhase old = m_group;
    if (old == phase)
        return {};
    m_group = phase;

    QList<Cue> cues;
    switch (phase) {
    case GroupPhase::Joining:
        if (old == GroupPhase::Idle || old == GroupPhase::Ended) {
            // A new call starts from nothing: whoever is there is the
            // baseline, and our own share cannot be live yet — KNOWN not
            // live, so the first share of the call is announced.
            m_roster.clear();
            m_localShareKnown = true;
            m_localSharing = false;
        }
        break;
    case GroupPhase::Connected:
        // Connected and reconnected are one cue: either way the user is now
        // in the call, and the roster that follows is a baseline.
        m_connectedAtMs = nowMs;
        if (old == GroupPhase::Joining || old == GroupPhase::Reconnecting)
            cues.append(Cue::Connected);
        break;
    case GroupPhase::Reconnecting:
        if (old == GroupPhase::Connected)
            cues.append(Cue::Disconnected);
        break;
    case GroupPhase::Idle:
    case GroupPhase::Ended:
        if (old == GroupPhase::Connected || old == GroupPhase::Reconnecting)
            cues.append(Cue::Ended);
        m_roster.clear();
        m_localShareKnown = true;
        m_localSharing = false;
        break;
    }
    return keepAllowed(cues, /*remote=*/false);
}

QList<Cue> Policy::rosterChanged(const Roster &remotes, qint64 nowMs)
{
    // Outside a settled, connected call every roster is a baseline: before
    // we connect, during a reconnect (people vanish and come back without
    // having gone anywhere), and for kSettleMs after connecting, when the
    // whole existing room arrives at once.
    if (m_group != GroupPhase::Connected
        || nowMs - m_connectedAtMs < kSettleMs) {
        m_roster = remotes;
        return {};
    }

    bool joined = false;
    bool left = false;
    bool handRaised = false;
    bool shareStarted = false;
    bool shareStopped = false;
    for (auto it = remotes.constBegin(); it != remotes.constEnd(); ++it) {
        const auto before = m_roster.constFind(it.key());
        if (before == m_roster.constEnd()) {
            joined = true;
            continue;
        }
        // Share and hand transitions only for someone present on both
        // sides: a person who arrives already sharing is announced by the
        // join, and one who leaves mid-share by the leave — never two cues
        // for one event.
        if (it->handRaised && !before->handRaised)
            handRaised = true;
        if (it->sharing && !before->sharing)
            shareStarted = true;
        if (!it->sharing && before->sharing)
            shareStopped = true;
    }
    for (auto it = m_roster.constBegin(); it != m_roster.constEnd(); ++it) {
        if (!remotes.contains(it.key())) {
            left = true;
            break;
        }
    }
    m_roster = remotes;

    const bool smallCall =
        remotes.size() + 1 <= kMaxCallSizeForPresence;
    QList<Cue> cues;
    // The allowed() test comes first so a suppressed cue does not consume
    // the throttle window of one that would have played.
    auto offer = [&](bool happened, Cue cue) {
        if (happened && allowed(cue, /*remote=*/true)
            && !throttled(cue, nowMs))
            cues.append(cue);
    };
    offer(joined && smallCall, Cue::Join);
    offer(left && smallCall, Cue::Leave);
    offer(shareStarted, Cue::ShareStart);
    offer(shareStopped, Cue::ShareStop);
    offer(handRaised, Cue::HandRaised);
    return cues;
}

QList<Cue> Policy::legacyPhaseChanged(LegacyPhase phase,
                                      bool remoteEndedOutgoing)
{
    const LegacyPhase old = m_legacy;
    if (old == phase)
        return {};
    m_legacy = phase;

    QList<Cue> cues;
    if (phase == LegacyPhase::Active)
        cues.append(Cue::Connected);
    if (phase == LegacyPhase::Ended || phase == LegacyPhase::Idle) {
        if (old == LegacyPhase::Connecting || old == LegacyPhase::Active)
            cues.append(Cue::Ended);
        else if (old == LegacyPhase::OutgoingRinging && remoteEndedOutgoing)
            cues.append(Cue::Ended);
    }
    return keepAllowed(cues, /*remote=*/false);
}

QList<Cue> Policy::localAudioChanged(Lane lane, bool micMuted, bool deafened)
{
    AudioState &state = lane == Lane::Group ? m_groupAudio : m_legacyAudio;
    const bool live = lane == Lane::Group ? groupLive() : legacyLive();
    const AudioState before = state;
    state.known = true;
    state.muted = micMuted;
    state.deafened = deafened;
    // Outside a call the controls are still settable (the lobby), and a
    // controller may reset them on the way out; neither is an action the
    // user needs to hear confirmed.
    if (!live || !before.known)
        return {};

    QList<Cue> cues;
    // Deafen implies mute and undeafen restores the previous mute, so one
    // press moves both flags. It is ONE action and gets ONE cue: the deafen
    // pair wins over the mute pair.
    if (deafened != before.deafened)
        cues.append(deafened ? Cue::Deafen : Cue::Undeafen);
    else if (micMuted != before.muted)
        cues.append(micMuted ? Cue::Mute : Cue::Unmute);
    return keepAllowed(cues, /*remote=*/false);
}

QList<Cue> Policy::localShareChanged(bool sharing)
{
    const bool wasKnown = m_localShareKnown;
    const bool was = m_localSharing;
    m_localShareKnown = true;
    m_localSharing = sharing;
    if (!groupLive() || !wasKnown || was == sharing)
        return {};
    return keepAllowed({ sharing ? Cue::ShareStart : Cue::ShareStop },
                       /*remote=*/false);
}

Loop Policy::desiredLoop() const
{
    if (m_incomingRing)
        return inCall() ? Loop::CallWaiting : Loop::Ring;
    if (m_legacy == LegacyPhase::OutgoingRinging && m_prefs.enabled
        && !m_outputCaptured)
        return Loop::Ringback;
    return Loop::None;
}

} // namespace callsound
