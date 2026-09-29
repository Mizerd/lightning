// The two rules AppController applies between the call lanes (review S1-S3):
// which refused MatrixRTC joins may fall back to a legacy call, and when a
// call failure may be withdrawn from the shared status strip.
#include <QtTest/QtTest>

#include "calls/CallLanePolicy.h"

using lightning::calls::CallFailureNotice;
using lightning::calls::LegacyFallbackArm;
using Phase = LegacyFallbackArm::Phase;

namespace {
const QString kDm = QStringLiteral("!dm:x");
} // namespace

class CallLanePolicyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The call button's join, refused as forbidden: falls back once.
    void theButtonsRefusedJoinFallsBackOnce()
    {
        LegacyFallbackArm arm;
        arm.phaseChanged(Phase::Preparing); // inside join()
        arm.buttonJoinDispatched(kDm, false, true);
        arm.phaseChanged(Phase::Failed); // teardown precedes the refusal
        const auto first = arm.take(kDm);
        QVERIFY(first.fallBack);
        QVERIFY(!first.video);
        QVERIFY(!arm.take(kDm).fallBack);
        // Another room's refusal is not this arm's.
        arm.buttonJoinDispatched(kDm, false, true);
        QVERIFY(!arm.take(QStringLiteral("!other:x")).fallBack);
        QVERIFY(!arm.armedFor(kDm));
        // A join that did not dispatch arms nothing; a video press says so.
        arm.buttonJoinDispatched(kDm, false, false);
        QVERIFY(!arm.armedFor(kDm));
        arm.buttonJoinDispatched(kDm, true, true);
        QVERIFY(arm.take(kDm).video);
    }

    // Review S1: a button join that ended some other way (hung up while
    // Preparing, a non-forbidden refusal) left the arm set, and a later
    // banner or card join refused as forbidden then placed a legacy call.
    void anyOtherJoinOrTheEndOfTheCallDisarmsIt()
    {
        for (const Phase phase : {Phase::Preparing, Phase::PastTheGate,
                                  Phase::Idle, Phase::Ended}) {
            LegacyFallbackArm arm;
            arm.buttonJoinDispatched(kDm, false, true);
            arm.phaseChanged(phase);
            QVERIFY2(!arm.take(kDm).fallBack,
                     qPrintable(QStringLiteral("still armed after phase %1")
                                    .arg(static_cast<int>(phase))));
        }
        // A failed button join (network) followed by a banner join.
        LegacyFallbackArm arm;
        arm.buttonJoinDispatched(kDm, false, true);
        arm.phaseChanged(Phase::Failed);
        QVERIFY(arm.armedFor(kDm));
        arm.phaseChanged(Phase::Preparing); // the banner's join
        QVERIFY(!arm.take(kDm).fallBack);
        // Navigation disarms too.
        arm.buttonJoinDispatched(kDm, false, true);
        arm.roomChanged();
        QVERIFY(!arm.armedFor(kDm));
    }

    // Review S2: an empty callFailed() is a withdrawal, and it must not wipe
    // a message somebody else put on the strip since.
    void anEmptyReasonWithdrawsOnlyOurOwnNotice()
    {
        CallFailureNotice notice;
        // Nothing of ours shown: an empty reason reports nothing at all.
        notice.reported(QStringLiteral("Couldn't send the message."));
        QVERIFY(!notice.failed(QString()).has_value());

        const auto report = notice.failed(QStringLiteral("Your camera stopped."));
        QCOMPARE(report.value_or(QStringLiteral("<none>")),
                 QStringLiteral("Your camera stopped."));
        notice.reported(*report);
        // Still ours on the strip: withdrawn with an empty report.
        QCOMPARE(notice.failed(QString()).value_or(QStringLiteral("<none>")),
                 QString());

        // Replaced by someone else's message: the withdrawal says nothing.
        const auto second = notice.failed(QStringLiteral("Call audio stopped."));
        notice.reported(*second);
        notice.reported(QStringLiteral("Upload failed."));
        QVERIFY(!notice.failed(QString()).has_value());
        QVERIFY(!notice.withdraw().has_value());
    }

    // Review S3: opening another room withdrew the notice of a call that was
    // still running ("Call audio stopped. Leave and rejoin ...").
    void navigationKeepsARunningCallsNotice()
    {
        CallFailureNotice notice;
        notice.reported(*notice.failed(QStringLiteral("Call audio stopped.")));
        QVERIFY(!notice.roomChanged(/*callRunning=*/true).has_value());
        // Still withdrawable once the call is over.
        QCOMPARE(notice.roomChanged(false).value_or(QStringLiteral("<none>")),
                 QString());
        QVERIFY(!notice.roomChanged(false).has_value());
    }
};

QTEST_GUILESS_MAIN(CallLanePolicyTest)
#include "CallLanePolicyTest.moc"
