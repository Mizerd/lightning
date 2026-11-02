
#include "media/VoiceRecorder.h"

#include <QtTest/QtTest>

class VoiceRecorderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void emptyAndDegenerateInputs()
    {
        QVERIFY(VoiceRecorder::bucketsFromPeaks({}, 100).isEmpty());
        QVERIFY(VoiceRecorder::bucketsFromPeaks({0.5f}, 0).isEmpty());
        QVERIFY(VoiceRecorder::bucketsFromPeaks({0.5f}, -3).isEmpty());
    }

    void shortRecordingKeepsOneBucketPerChunk()
    {
        const QList<int> out =
            VoiceRecorder::bucketsFromPeaks({0.25f, 0.5f, 1.0f}, 100);
        QCOMPARE(out.size(), 3);
        QCOMPARE(out.at(2), 100);
        QCOMPARE(out.at(1), 50);
        QCOMPARE(out.at(0), 25);
    }

    void longRecordingDownsamplesMaxPreserving()
    {
        QList<float> peaks(10, 0.1f);
        peaks[3] = 1.0f;
        const QList<int> out = VoiceRecorder::bucketsFromPeaks(peaks, 5);
        QCOMPARE(out.size(), 5);
        QCOMPARE(out.at(1), 100);
        for (int b : {0, 2, 3, 4})
            QCOMPARE(out.at(b), 10);
    }

    void silenceStaysFlatZero()
    {
        const QList<int> out =
            VoiceRecorder::bucketsFromPeaks({0.0f, 0.0f, 0.0f}, 100);
        QCOMPARE(out.size(), 3);
        for (int v : out)
            QCOMPARE(v, 0);
    }

    void valuesAreAlwaysWithinBridgeBounds()
    {
        QList<float> peaks;
        for (int i = 0; i < 500; ++i)
            peaks.append(i % 7 == 0 ? 3.5f : 0.4f);
        const QList<int> out = VoiceRecorder::bucketsFromPeaks(
            peaks, VoiceRecorder::kMaxWaveformBuckets);
        QCOMPARE(out.size(), VoiceRecorder::kMaxWaveformBuckets);
        for (int v : out) {
            QVERIFY(v >= 0);
            QVERIFY(v <= 100);
        }
    }
};

QTEST_GUILESS_MAIN(VoiceRecorderTest)
#include "VoiceRecorderTest.moc"
