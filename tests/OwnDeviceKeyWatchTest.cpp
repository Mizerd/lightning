// The decisions around "this device can never decrypt anything": a device
// that publishes a curve25519 identity key its own Olm account does not hold
// receives nothing decryptable, while sending keeps working.
//
// The check costs a /keys/query and may not answer, so:
//
//   * Unknown is neither a fault nor a clearance: an offline user is not told
//     their encryption is broken, and a broken device is not declared healthy
//     because a later check timed out.
//   * The re-check is rate limited: the fault can appear after login, but the
//     four event-driven callers fire in one burst.
//
// No key material appears here; the tri-state carries only whether two keys
// agree.

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

    // An unanswerable check (offline, keys not uploaded yet, a 5xx on
    // /keys/query) leaves a healthy user alone.
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

    // Once broken, a later "could not establish" does not report the device
    // healthy again.
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

    // The burst gate: sign-in, first sync, a verification and an explicit
    // refresh can land in one second and share one minimum gap.
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

    // The periodic backstop's interval must clear the burst gate, or the
    // timer could never fire.
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
