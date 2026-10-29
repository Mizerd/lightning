#include <QTest>
#include "calls/MacAudioTimeline.h"

using lightning::macaudio::CaptureTimeline;

class MacAudioTimelineTest : public QObject {
    Q_OBJECT
private slots:
    void deliveryJitterDoesNotChangeSampleTime()
    {
        CaptureTimeline timeline;
        const qint64 epoch = 10'000'000'000;
        // A scheduler stall followed by a burst of callbacks. Arrival times
        // stay increasing while capture spacing stays exactly 512 samples.
        const qint64 delays[] = {10'000'000, 60'000'000, 50'000'000, 40'000'000};
        for (int i = 0; i < 4; ++i) {
            const qint64 captured = epoch + qint64(i) * 512 * 1'000'000'000 / 48000;
            const auto pts = timeline.map(captured, captured + delays[i], captured + delays[i] - epoch);
            QVERIFY(pts.has_value());
            QCOMPARE(*pts, quint64(captured - epoch));
        }
    }
    void joiningAnExistingCallUsesItsRunningTime()
    {
        CaptureTimeline timeline;
        QCOMPARE(*timeline.map(10'000'000'000, 10'010'000'000, 120'000'000'000),
                 std::uint64_t(119'990'000'000));
    }
    void staleAudioIsDroppedInsteadOfRetimestamped()
    {
        CaptureTimeline timeline;
        QVERIFY(!timeline.map(1'000'000'000, 1'200'000'000, 500'000'000));
        QCOMPARE(*timeline.map(1'200'000'000, 1'210'000'000, 510'000'000),
                 std::uint64_t(500'000'000));
    }
    void capturedBeforePipelineStartIsNotWrapped()
    {
        CaptureTimeline timeline;
        QVERIFY(!timeline.map(1'000'000'000, 1'010'000'000, 5'000'000));
        QVERIFY(timeline.map(1'010'000'000, 1'020'000'000, 15'000'000));
    }
    void backwardsAndFutureSamplesAreRejected()
    {
        CaptureTimeline timeline;
        QVERIFY(timeline.map(1'000'000'000, 1'010'000'000, 100'000'000));
        QVERIFY(!timeline.map(999'000'000, 1'010'000'000, 100'000'000));
        QVERIFY(!timeline.map(1'020'000'000, 1'010'000'000, 100'000'000));
    }
};
QTEST_GUILESS_MAIN(MacAudioTimelineTest)
#include "MacAudioTimelineTest.moc"
