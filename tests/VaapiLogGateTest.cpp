#include "media/VaapiLogGate.h"

#include <QtTest>

// The VAAPI warning gate: Qt's FFmpeg backend can emit
// "vaExportSurfaceHandle failed" / "failed to get textures for frame" once
// per frame. The gate passes the first occurrences, then drops the storm with
// periodic summaries, and never touches other messages.
class VaapiLogGateTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void unrelatedMessagesAlwaysPrintAndAreNotCounted()
    {
        VaapiLogGate gate;
        for (int i = 0; i < 100; ++i) {
            QCOMPARE(gate.classify(QStringLiteral("ordinary log line %1").arg(i)),
                     VaapiLogGate::Action::Print);
        }
        QCOMPARE(gate.seen(), Q_INT64_C(0));
    }

    void recognizesBothSpamPatterns()
    {
        QVERIFY(VaapiLogGate::matches(
            QStringLiteral("vaExportSurfaceHandle failed")));
        QVERIFY(VaapiLogGate::matches(QStringLiteral(
            "qt.multimedia.ffmpeg: failed to get textures for frame; format: 44")));
        QVERIFY(!VaapiLogGate::matches(QStringLiteral("textures loaded fine")));
    }

    void firstOccurrencesPassThenStormIsDroppedWithPeriodicSummaries()
    {
        VaapiLogGate gate;
        const QString spam = QStringLiteral("vaExportSurfaceHandle failed");
        int printed = 0;
        int summaries = 0;
        int dropped = 0;
        for (int i = 0; i < 1200; ++i) {
            switch (gate.classify(spam)) {
            case VaapiLogGate::Action::Print: ++printed; break;
            case VaapiLogGate::Action::Summary: ++summaries; break;
            case VaapiLogGate::Action::Drop: ++dropped; break;
            }
        }
        QCOMPARE(printed, int(VaapiLogGate::kPassThrough));
        QCOMPARE(summaries, 2); // occurrences 500 and 1000
        QCOMPARE(dropped, 1200 - printed - summaries);
        QCOMPARE(gate.seen(), Q_INT64_C(1200));
        QVERIFY(gate.summaryLine().contains(QStringLiteral("1200")));
        QVERIFY(gate.summaryLine().contains(
            QStringLiteral("QT_DISABLE_HW_TEXTURES_CONVERSION")));
    }
};

QTEST_GUILESS_MAIN(VaapiLogGateTest)
#include "VaapiLogGateTest.moc"
