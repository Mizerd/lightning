// B011: the two decisions around "this device can never decrypt anything".
//
// The fault (audit B006, diagnosed on a real account 2026-09-07): a device
// published a curve25519 identity key its own local Olm account did not hold,
// so every peer encrypted to a key it could not read. Nothing arrived
// decryptable, ever, while sending kept working.
//
// The check that names it costs a /keys/query and can fail to answer at all.
// Two properties therefore have to hold, and both are cheap to get wrong:
//
//   * UNKNOWN IS NEITHER A FAULT NOR A CLEARANCE. Telling a user who is
//     merely offline that their encryption is destroyed is worse than saying
//     nothing; silently declaring a broken device healthy because a later
//     check timed out is worse still.
//   * THE RE-CHECK IS RATE LIMITED. The fault can appear after login, so one
//     check at sign-in is not enough — but a /keys/query per minute is not
//     acceptable, and the four event-driven callers fire in one burst.
//
// No key material appears anywhere in this suite; the tri-state carries only
// whether two keys agree.

#include "crypto/OwnDeviceKeyWatch.h"

#include <QtTest/QtTest>

using matrix::crypto::KeyAgreement;
using matrix::crypto::OwnDeviceKeyWatch;

class OwnDeviceKeyWatchTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A fresh session knows nothing, and "nothing" is not a fault.
    void aFreshWatchIsNeitherBrokenNorAnswered()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY(!watch.broken());
        QVERIFY(!watch.answered());
    }

    // THE CASE THAT MUST NEVER REGRESS: an unanswerable check (offline, keys
    // not uploaded yet, a 5xx on /keys/query) leaves a healthy user alone.
    void unknownNeverRaisesAFault()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY2(!watch.apply(KeyAgreement::Unknown),
                 "an unanswerable check is not a state change");
        QVERIFY2(!watch.broken(),
                 "\"could not be established\" was reported as a broken "
                 "device — that tells a user who is merely offline that "
                 "their encryption is destroyed");
        QVERIFY2(!watch.answered(),
                 "an unanswerable check must not count as an answer");
    }

    // The real fault, and it is announced exactly once.
    void anExplicitMismatchLatchesTheFaultOnce()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY2(watch.apply(KeyAgreement::Mismatch),
                 "the first mismatch is a transition and must be announced");
        QVERIFY(watch.broken());
        QVERIFY(watch.answered());
        QVERIFY2(!watch.apply(KeyAgreement::Mismatch),
                 "a repeat answer is not a transition — the 15-minute "
                 "backstop must not re-announce the same fault");
        QVERIFY(watch.broken());
    }

    // AND THE OTHER HALF OF THE TRI-STATE: once broken, a later "could not
    // establish" must not quietly report the device healthy again.
    void unknownAfterAFaultDoesNotClearIt()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY(watch.apply(KeyAgreement::Mismatch));
        // broken() FIRST: it is the assertion whose failure names the real
        // consequence, and a QVERIFY that fails earlier would shadow it.
        const bool announced = watch.apply(KeyAgreement::Unknown);
        QVERIFY2(watch.broken(),
                 "a check that could not run cleared a real fault — the "
                 "device is still undecryptable and the user would be told "
                 "it is fine");
        QVERIFY2(!announced,
                 "unknown is not a state change in either direction");
    }

    // A genuine agreement DOES clear it. Signing in again is the repair, and
    // the same controller instance must be able to report the new session
    // healthy rather than staying stuck on the old fault.
    void anExplicitMatchClearsTheFault()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY(watch.apply(KeyAgreement::Mismatch));
        QVERIFY2(watch.apply(KeyAgreement::Matches),
                 "recovering is a transition and must be announced");
        QVERIFY(!watch.broken());
        QVERIFY(watch.answered());
    }

    // A healthy answer on a healthy watch is not a transition.
    void repeatedHealthyAnswersAreNotTransitions()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY2(!watch.apply(KeyAgreement::Matches),
                 "the watch starts not-broken, so \"matches\" changes "
                 "nothing about broken()");
        QVERIFY(watch.answered());
        QVERIFY(!watch.broken());
    }

    // reset() is what a sign-out / account switch runs: the next account must
    // not inherit the previous one's fault, and the rate limit must not make
    // its first check wait.
    void resetForgetsTheFaultAndTheRateLimit()
    {
        OwnDeviceKeyWatch watch;
        watch.noteDispatched(1000);
        QVERIFY(watch.apply(KeyAgreement::Mismatch));
        watch.reset();
        QVERIFY(!watch.broken());
        QVERIFY(!watch.answered());
        QVERIFY2(watch.checkDue(1000),
                 "a new session's first check must not be held off by the "
                 "previous session's dispatch time");
    }

    // THE BURST GATE. Sign-in, first sync, a verification and an explicit
    // refresh can all land inside one second; they share one minimum gap.
    void checksInsideTheMinimumGapAreRefused()
    {
        OwnDeviceKeyWatch watch;
        const qint64 t0 = 5'000'000;
        QVERIFY2(watch.checkDue(t0), "the first check of a session is due");
        watch.noteDispatched(t0);
        QVERIFY2(!watch.checkDue(t0), "a second check in the same instant");
        QVERIFY2(!watch.checkDue(t0 + 1000),
                 "four event-driven callers within a second must cost one "
                 "/keys/query, not four");
        QVERIFY2(!watch.checkDue(t0 + OwnDeviceKeyWatch::kMinIntervalMs - 1),
                 "the gap is closed right up to its boundary");
        QVERIFY2(watch.checkDue(t0 + OwnDeviceKeyWatch::kMinIntervalMs),
                 "and open exactly at it");
    }

    // The periodic backstop must actually be able to fire: its interval has
    // to clear the burst gate, or the timer would be a permanent no-op — the
    // recorded failure mode of the row window (§16), where a policy shipped
    // guarded on a condition its only call site could never satisfy.
    void theBackstopIntervalClearsTheBurstGate()
    {
        QVERIFY2(OwnDeviceKeyWatch::kRecheckIntervalMs
                     > OwnDeviceKeyWatch::kMinIntervalMs,
                 "the periodic re-check would be refused by the rate limit "
                 "at every tick");
        OwnDeviceKeyWatch watch;
        const qint64 t0 = 5'000'000;
        watch.noteDispatched(t0);
        QVERIFY(watch.checkDue(t0 + OwnDeviceKeyWatch::kRecheckIntervalMs));
    }

    // And it must stay cheap: no more than four single-device /keys/query
    // requests an hour from the backstop.
    void theBackstopIsCheap()
    {
        QVERIFY2(OwnDeviceKeyWatch::kRecheckIntervalMs >= 5 * 60 * 1000,
                 "a /keys/query every few minutes is not an acceptable "
                 "background cost");
    }

    // A clock step (suspend/resume, NTP) must not wedge the watch until wall
    // time catches up.
    void aBackwardsClockDoesNotWedgeTheWatch()
    {
        OwnDeviceKeyWatch watch;
        watch.noteDispatched(10'000'000);
        QVERIFY(watch.checkDue(9'000'000));
    }
};

QTEST_MAIN(OwnDeviceKeyWatchTest)
#include "OwnDeviceKeyWatchTest.moc"
