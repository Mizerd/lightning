// Which call sound plays when — CallSoundPolicy on its own, no audio.
//
// Every case here is a RULE the policy exists to keep, and each asserts the
// exact cue list, never just "something played": a list that is merely
// non-empty cannot say WHICH branch produced it (§16, the ShortcutRegistry
// lesson).
#include <QtTest/QtTest>

#include "calls/CallSoundPolicy.h"

using namespace callsound;

namespace {

Roster roster(std::initializer_list<QString> identities)
{
    Roster r;
    for (const QString &id : identities)
        r.insert(id, RemoteParticipant{});
    return r;
}

/// A policy already settled in a connected group call with `remotes`.
Policy connectedWith(const Roster &remotes, qint64 &clock)
{
    Policy p;
    p.localAudioChanged(Lane::Group, false, false);
    p.groupPhaseChanged(GroupPhase::Joining, clock);
    p.groupPhaseChanged(GroupPhase::Connected, clock);
    p.rosterChanged(remotes, clock);
    clock += Policy::kSettleMs + 1;
    return p;
}

} // namespace

class CallSoundPolicyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void joiningAndLeavingAnnounceYourself()
    {
        Policy p;
        QCOMPARE(p.groupPhaseChanged(GroupPhase::Joining, 0), QList<Cue>{});
        QCOMPARE(p.groupPhaseChanged(GroupPhase::Connected, 100),
                 QList<Cue>{ Cue::Connected });
        QCOMPARE(p.groupPhaseChanged(GroupPhase::Ended, 5000),
                 QList<Cue>{ Cue::Ended });
    }

    // Element Web's rule: nothing had started, so nothing "ends".
    void aJoinThatNeverConnectedEndsSilently()
    {
        Policy p;
        p.groupPhaseChanged(GroupPhase::Joining, 0);
        QCOMPARE(p.groupPhaseChanged(GroupPhase::Ended, 10), QList<Cue>{});
    }

    // THE CHORUS. The people already in the room arrive as one roster at
    // connect; announcing them would play a join per person.
    void theRoomAlreadyThereIsNotAnnounced()
    {
        Policy p;
        p.groupPhaseChanged(GroupPhase::Joining, 0);
        p.groupPhaseChanged(GroupPhase::Connected, 1000);
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c" }), 1100),
                 QList<Cue>{});
        // Still inside the settle window: a straggler is baseline too.
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c", "d" }),
                                 1000 + Policy::kSettleMs - 1),
                 QList<Cue>{});
        // After it, a real arrival is news.
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c", "d", "e" }),
                                 1000 + Policy::kSettleMs + 10),
                 QList<Cue>{ Cue::Join });
    }

    void someoneJoiningAndLeavingIsAnnounced()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        QCOMPARE(p.rosterChanged(roster({ "a", "b" }), t),
                 QList<Cue>{ Cue::Join });
        t += 1000;
        QCOMPARE(p.rosterChanged(roster({ "a" }), t),
                 QList<Cue>{ Cue::Leave });
    }

    // Several people in one update are ONE cue, and a burst within the
    // throttle window is one cue too.
    void aBurstIsOneCue()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c" }), t),
                 QList<Cue>{ Cue::Join });
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c", "d" }),
                                 t + Policy::kPresenceThrottleMs - 1),
                 QList<Cue>{});
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c", "d", "e" }),
                                 t + Policy::kPresenceThrottleMs + 1),
                 QList<Cue>{ Cue::Join });
    }

    void aLargeCallIsQuiet()
    {
        qint64 t = 0;
        // Seven others plus us: eight, still announced.
        Policy p = connectedWith(roster({ "a", "b", "c", "d", "e", "f" }), t);
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c", "d", "e", "f", "g" }),
                                 t),
                 QList<Cue>{ Cue::Join });
        t += 1000;
        // Nine: silent.
        QCOMPARE(p.rosterChanged(
                     roster({ "a", "b", "c", "d", "e", "f", "g", "h" }), t),
                 QList<Cue>{});
    }

    // A reconnect drops and re-adds people who never went anywhere.
    void aReconnectIsNotEveryoneLeavingAndComingBack()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a", "b" }), t);
        QCOMPARE(p.groupPhaseChanged(GroupPhase::Reconnecting, t),
                 QList<Cue>{ Cue::Disconnected });
        QCOMPARE(p.rosterChanged(roster({}), t + 10), QList<Cue>{});
        t += 3000;
        QCOMPARE(p.groupPhaseChanged(GroupPhase::Connected, t),
                 QList<Cue>{ Cue::Connected });
        QCOMPARE(p.rosterChanged(roster({ "a", "b" }), t + 100),
                 QList<Cue>{});
    }

    void muteAndUnmuteAreConfirmedInACall()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        QCOMPARE(p.localAudioChanged(Lane::Group, true, false),
                 QList<Cue>{ Cue::Mute });
        QCOMPARE(p.localAudioChanged(Lane::Group, false, false),
                 QList<Cue>{ Cue::Unmute });
    }

    // Deafen moves BOTH flags (it mutes too). One press, one cue.
    void deafenIsOneCueNotTwo()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        QCOMPARE(p.localAudioChanged(Lane::Group, true, true),
                 QList<Cue>{ Cue::Deafen });
        QCOMPARE(p.localAudioChanged(Lane::Group, false, false),
                 QList<Cue>{ Cue::Undeafen });
    }

    // The lobby, and a controller resetting its flags on the way out, are
    // not actions to confirm.
    void controlsOutsideACallAreSilent()
    {
        Policy p;
        QCOMPARE(p.localAudioChanged(Lane::Group, false, false),
                 QList<Cue>{});
        QCOMPARE(p.localAudioChanged(Lane::Group, true, false),
                 QList<Cue>{});
        QCOMPARE(p.localAudioChanged(Lane::Legacy, true, true),
                 QList<Cue>{});
    }

    // DEAFENED: nothing other people do makes a sound; your own hands still
    // confirm.
    void deafenSilencesTheRoomButNotYourOwnActions()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        p.localAudioChanged(Lane::Group, true, true);
        QVERIFY(p.deafened());
        QCOMPARE(p.rosterChanged(roster({ "a", "b" }), t), QList<Cue>{});
        Roster sharing = roster({ "a", "b" });
        sharing["a"].sharing = true;
        sharing["a"].handRaised = true;
        QCOMPARE(p.rosterChanged(sharing, t + 1000), QList<Cue>{});
        QCOMPARE(p.localShareChanged(true), QList<Cue>{ Cue::ShareStart });
        QCOMPARE(p.localAudioChanged(Lane::Group, true, false),
                 QList<Cue>{ Cue::Undeafen });
        // And the room is audible again. `a` is still sharing and still has
        // a hand up — a roster that dropped those flags would be a genuine
        // share STOP and must play one, which is not what this case is about.
        sharing.insert(QStringLiteral("c"), RemoteParticipant{});
        QCOMPARE(p.rosterChanged(sharing, t + 2000),
                 QList<Cue>{ Cue::Join });
    }

    void sharesAndHandsOfOthers()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a", "b" }), t);
        Roster r = roster({ "a", "b" });
        r["a"].sharing = true;
        QCOMPARE(p.rosterChanged(r, t), QList<Cue>{ Cue::ShareStart });
        r["b"].handRaised = true;
        QCOMPARE(p.rosterChanged(r, t + 1000),
                 QList<Cue>{ Cue::HandRaised });
        // Lowering a hand is not an event anybody needs to hear.
        r["b"].handRaised = false;
        QCOMPARE(p.rosterChanged(r, t + 2000), QList<Cue>{});
        r["a"].sharing = false;
        QCOMPARE(p.rosterChanged(r, t + 3000), QList<Cue>{ Cue::ShareStop });
    }

    // A sharer who leaves is ONE event: the leave.
    void leavingMidShareIsOnlyALeave()
    {
        qint64 t = 0;
        Roster r = roster({ "a", "b" });
        r["a"].sharing = true;
        Policy p = connectedWith(r, t);
        QCOMPARE(p.rosterChanged(roster({ "b" }), t),
                 QList<Cue>{ Cue::Leave });
    }

    void yourOwnShareIsAnnouncedFromTheFirstOne()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        QCOMPARE(p.localShareChanged(true), QList<Cue>{ Cue::ShareStart });
        QCOMPARE(p.localShareChanged(true), QList<Cue>{});
        QCOMPARE(p.localShareChanged(false), QList<Cue>{ Cue::ShareStop });
    }

    void eachSwitchSilencesItsOwnCategory()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        Preferences prefs;
        prefs.presence = false;
        p.setPreferences(prefs);
        QCOMPARE(p.rosterChanged(roster({ "a", "b" }), t), QList<Cue>{});
        QCOMPARE(p.localAudioChanged(Lane::Group, true, false),
                 QList<Cue>{ Cue::Mute });

        prefs = Preferences{};
        prefs.controls = false;
        p.setPreferences(prefs);
        QCOMPARE(p.localAudioChanged(Lane::Group, false, false),
                 QList<Cue>{});
        QCOMPARE(p.localShareChanged(true), QList<Cue>{ Cue::ShareStart });

        prefs = Preferences{};
        prefs.shareAndHand = false;
        p.setPreferences(prefs);
        QCOMPARE(p.localShareChanged(false), QList<Cue>{});
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c" }), t + 1000),
                 QList<Cue>{ Cue::Join });

        prefs = Preferences{};
        prefs.enabled = false;
        p.setPreferences(prefs);
        QCOMPARE(p.groupPhaseChanged(GroupPhase::Ended, t + 2000),
                 QList<Cue>{});
    }

    // A cue a switch suppressed must not use up the throttle window of the
    // next one that would have played.
    void aSuppressedCueDoesNotConsumeTheThrottle()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        p.localAudioChanged(Lane::Group, true, true); // deafened
        QCOMPARE(p.rosterChanged(roster({ "a", "b" }), t), QList<Cue>{});
        p.localAudioChanged(Lane::Group, false, false);
        QCOMPARE(p.rosterChanged(roster({ "a", "b", "c" }), t + 10),
                 QList<Cue>{ Cue::Join });
    }

    // A share carrying the whole output mix would carry our cues into the
    // call; while it is live, nothing plays.
    void nothingLeaksIntoACapturedOutputMix()
    {
        qint64 t = 0;
        Policy p = connectedWith(roster({ "a" }), t);
        p.setOutputCapturedByShare(true);
        QCOMPARE(p.rosterChanged(roster({ "a", "b" }), t), QList<Cue>{});
        QCOMPARE(p.localAudioChanged(Lane::Group, true, false),
                 QList<Cue>{});
        p.setOutputCapturedByShare(false);
        QCOMPARE(p.localAudioChanged(Lane::Group, false, false),
                 QList<Cue>{ Cue::Unmute });
    }

    // ── Loops ─────────────────────────────────────────────────────────────

    void anIncomingRingRingsAndBecomesCallWaitingInACall()
    {
        Policy p;
        QCOMPARE(p.desiredLoop(), Loop::None);
        p.setIncomingRing(true);
        QCOMPARE(p.desiredLoop(), Loop::Ring);
        p.groupPhaseChanged(GroupPhase::Joining, 0);
        p.groupPhaseChanged(GroupPhase::Connected, 10);
        QCOMPARE(p.desiredLoop(), Loop::CallWaiting);
        p.setIncomingRing(false);
        QCOMPARE(p.desiredLoop(), Loop::None);
    }

    // The ring is governed by the ring switch, applied by whoever announces
    // it — not by the in-call master switch, and not by deafen.
    void theRingIgnoresTheInCallSwitches()
    {
        Policy p;
        Preferences off;
        off.enabled = false;
        p.setPreferences(off);
        p.setIncomingRing(true);
        QCOMPARE(p.desiredLoop(), Loop::Ring);
    }

    void legacyOutgoingCallRingsBackAndEnds()
    {
        Policy p;
        QCOMPARE(p.legacyPhaseChanged(LegacyPhase::OutgoingRinging, false),
                 QList<Cue>{});
        QCOMPARE(p.desiredLoop(), Loop::Ringback);
        QCOMPARE(p.legacyPhaseChanged(LegacyPhase::Connecting, false),
                 QList<Cue>{});
        QCOMPARE(p.desiredLoop(), Loop::None);
        QCOMPARE(p.legacyPhaseChanged(LegacyPhase::Active, false),
                 QList<Cue>{ Cue::Connected });
        QCOMPARE(p.legacyPhaseChanged(LegacyPhase::Ended, true),
                 QList<Cue>{ Cue::Ended });
        QCOMPARE(p.legacyPhaseChanged(LegacyPhase::Idle, false),
                 QList<Cue>{});
    }

    void legacyOutgoingDeclinedEndsAudiblyCancelledSilently()
    {
        Policy declined;
        declined.legacyPhaseChanged(LegacyPhase::OutgoingRinging, false);
        QCOMPARE(declined.legacyPhaseChanged(LegacyPhase::Ended, true),
                 QList<Cue>{ Cue::Ended });

        Policy cancelled;
        cancelled.legacyPhaseChanged(LegacyPhase::OutgoingRinging, false);
        QCOMPARE(cancelled.legacyPhaseChanged(LegacyPhase::Ended, false),
                 QList<Cue>{});
    }

    void aRingThatEndsUnansweredIsSilent()
    {
        Policy p;
        p.legacyPhaseChanged(LegacyPhase::IncomingRinging, false);
        QCOMPARE(p.legacyPhaseChanged(LegacyPhase::Ended, true),
                 QList<Cue>{});
    }

    void theRingbackStopsWhenTheMasterSwitchGoesOff()
    {
        Policy p;
        p.legacyPhaseChanged(LegacyPhase::OutgoingRinging, false);
        QCOMPARE(p.desiredLoop(), Loop::Ringback);
        Preferences off;
        off.enabled = false;
        p.setPreferences(off);
        QCOMPARE(p.desiredLoop(), Loop::None);
    }

    // Every cue and loop names a file the generator renders; an empty or
    // duplicated name would be a sound that can never play.
    void everySoundHasADistinctName()
    {
        QSet<QString> names;
        const QList<Cue> cues = {
            Cue::Join, Cue::Leave, Cue::Connected, Cue::Ended,
            Cue::Disconnected, Cue::Mute, Cue::Unmute, Cue::Deafen,
            Cue::Undeafen, Cue::ShareStart, Cue::ShareStop, Cue::HandRaised,
        };
        for (Cue cue : cues)
            names.insert(soundName(cue));
        for (Loop loop : { Loop::Ring, Loop::CallWaiting, Loop::Ringback })
            names.insert(soundName(loop));
        QVERIFY(!names.contains(QString()));
        QCOMPARE(names.size(), cues.size() + 3);
        QCOMPARE(soundName(Loop::None), QString());
    }
};

QTEST_GUILESS_MAIN(CallSoundPolicyTest)
#include "CallSoundPolicyTest.moc"
