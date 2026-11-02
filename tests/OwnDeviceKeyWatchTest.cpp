
#include "crypto/OwnDeviceKeyWatch.h"

#include <QtTest/QtTest>

using matrix::crypto::KeyAgreement;
using matrix::crypto::OwnDeviceKeyWatch;

class OwnDeviceKeyWatchTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aFreshWatchIsNeitherBrokenNorAnswered()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY(!watch.broken());
        QVERIFY(!watch.answered());
    }

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

    void unknownAfterAFaultDoesNotClearIt()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY(watch.apply(KeyAgreement::Mismatch));
        const bool announced = watch.apply(KeyAgreement::Unknown);
        QVERIFY2(watch.broken(),
                 "a check that could not run cleared a real fault — the "
                 "device is still undecryptable and the user would be told "
                 "it is fine");
        QVERIFY2(!announced,
                 "unknown is not a state change in either direction");
    }

    void anExplicitMatchClearsTheFault()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY(watch.apply(KeyAgreement::Mismatch));
        QVERIFY2(watch.apply(KeyAgreement::Matches),
                 "recovering is a transition and must be announced");
        QVERIFY(!watch.broken());
        QVERIFY(watch.answered());
    }

    void repeatedHealthyAnswersAreNotTransitions()
    {
        OwnDeviceKeyWatch watch;
        QVERIFY2(!watch.apply(KeyAgreement::Matches),
                 "the watch starts not-broken, so \"matches\" changes "
                 "nothing about broken()");
        QVERIFY(watch.answered());
        QVERIFY(!watch.broken());
    }

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

    void theBackstopIsCheap()
    {
        QVERIFY2(OwnDeviceKeyWatch::kRecheckIntervalMs >= 5 * 60 * 1000,
                 "a /keys/query every few minutes is not an acceptable "
                 "background cost");
    }

    void aBackwardsClockDoesNotWedgeTheWatch()
    {
        OwnDeviceKeyWatch watch;
        watch.noteDispatched(10'000'000);
        QVERIFY(watch.checkDue(9'000'000));
    }
};

QTEST_MAIN(OwnDeviceKeyWatchTest)
#include "OwnDeviceKeyWatchTest.moc"
