#include <QtTest/QtTest>

#include "calls/CallLanePolicy.h"
#include "calls/OneToOneDm.h"

using lightning::calls::CallFailureNotice;
using lightning::calls::LegacyFallbackArm;
using Phase = LegacyFallbackArm::Phase;

namespace {
const QString kDm = QStringLiteral("!dm:x");
}

class CallLanePolicyTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void legacyFallbackRequiresAKnownOneToOneRoster()
    {
        RoomInfo room;
        room.isDirect = true;
        room.directUserIds = {QStringLiteral("@peer:x")};
        for (int count : {0, 3, -1}) {
            room.activeMemberCount = count;
            QVERIFY(::calls::oneToOneDmPeer(room, true).isEmpty());
        }
        room.activeMemberCount = 2;
        QCOMPARE(::calls::oneToOneDmPeer(room, true), QStringLiteral("@peer:x"));
        room.activeMemberCount = 0;
        QCOMPARE(::calls::oneToOneDmPeer(room), QStringLiteral("@peer:x"));
    }

    void theButtonsRefusedJoinFallsBackOnce()
    {
        LegacyFallbackArm arm;
        arm.phaseChanged(Phase::Preparing);
        arm.buttonJoinDispatched(kDm, false, true);
        arm.phaseChanged(Phase::Failed);
        const auto first = arm.take(kDm);
        QVERIFY(first.fallBack);
        QVERIFY(!first.video);
        QVERIFY(!arm.take(kDm).fallBack);
        arm.buttonJoinDispatched(kDm, false, true);
        QVERIFY(!arm.take(QStringLiteral("!other:x")).fallBack);
        QVERIFY(!arm.armedFor(kDm));
        arm.buttonJoinDispatched(kDm, false, false);
        QVERIFY(!arm.armedFor(kDm));
        arm.buttonJoinDispatched(kDm, true, true);
        QVERIFY(arm.take(kDm).video);
    }

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
        LegacyFallbackArm arm;
        arm.buttonJoinDispatched(kDm, false, true);
        arm.phaseChanged(Phase::Failed);
        QVERIFY(arm.armedFor(kDm));
        arm.phaseChanged(Phase::Preparing);
        QVERIFY(!arm.take(kDm).fallBack);
        arm.buttonJoinDispatched(kDm, false, true);
        arm.roomChanged();
        QVERIFY(!arm.armedFor(kDm));
    }

    void anEmptyReasonWithdrawsOnlyOurOwnNotice()
    {
        CallFailureNotice notice;
        notice.reported(QStringLiteral("Couldn't send the message."));
        QVERIFY(!notice.failed(QString()).has_value());

        const auto report = notice.failed(QStringLiteral("Your camera stopped."));
        QCOMPARE(report.value_or(QStringLiteral("<none>")),
                 QStringLiteral("Your camera stopped."));
        notice.reported(*report);
        QCOMPARE(notice.failed(QString()).value_or(QStringLiteral("<none>")),
                 QString());

        const auto second = notice.failed(QStringLiteral("Call audio stopped."));
        notice.reported(*second);
        notice.reported(QStringLiteral("Upload failed."));
        QVERIFY(!notice.failed(QString()).has_value());
        QVERIFY(!notice.withdraw().has_value());
    }

    void navigationKeepsARunningCallsNotice()
    {
        CallFailureNotice notice;
        notice.reported(*notice.failed(QStringLiteral("Call audio stopped.")));
        QVERIFY(!notice.roomChanged(true).has_value());
        QCOMPARE(notice.roomChanged(false).value_or(QStringLiteral("<none>")),
                 QString());
        QVERIFY(!notice.roomChanged(false).has_value());
    }
};

QTEST_GUILESS_MAIN(CallLanePolicyTest)
#include "CallLanePolicyTest.moc"
