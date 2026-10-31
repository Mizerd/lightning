#include "calls/StreamInfoStats.h"
#include <QtTest>

class StreamInfoStatsTest : public QObject
{
    Q_OBJECT
private slots:
    void deltasUseElapsedTimeAndRejectResets()
    {
        using lightning::calls::StreamInfoStats;
        QCOMPARE(StreamInfoStats::rate(1100, 100, 1000, 8), 8.0);
        QCOMPARE(StreamInfoStats::rate(2100, 100, 500, 8), 32.0);
        QCOMPARE(StreamInfoStats::rate(42, 12, 1000, 1000), 30.0);
        QCOMPARE(StreamInfoStats::rate(100, 100, 1000, 8), 0.0);
        QCOMPARE(StreamInfoStats::rate(1, 100, 1000, 8), -1.0);
        QCOMPARE(StreamInfoStats::rate(100, 0, 0, 8), -1.0);
        QCOMPARE(StreamInfoStats::rate(100, 0, -1, 8), -1.0);
    }
    // Measured live: "500.0 FPS" on one row and "0.0 FPS, 0 kbps" on another,
    // because a second poll landed 2 ms after the first.
    void aSecondPollInTheSameTickChangesNoRate()
    {
        lightning::calls::StreamInfoStats stats;
        stats.sample(0);
        stats.frames = 30;
        stats.bytes = 125000;
        auto row = stats.sample(1000);
        QCOMPARE(row.value("fps").toDouble(), 30.0);
        stats.frames += 1;
        row = stats.sample(1002);
        QCOMPARE(row.value("fps").toDouble(), 30.0);
        QCOMPARE(row.value("kbps").toDouble(), 1000.0);
        // The early poll did not restart the window.
        stats.frames = 60;
        row = stats.sample(2000);
        QCOMPARE(row.value("fps").toDouble(), 30.0);
    }

    void snapshotsParseCountersAndRestartWithAnUnknownRate()
    {
        lightning::calls::StreamInfoStats stats;
        stats.outgoing = true;
        stats.screen = true;
        stats.source = QStringLiteral("videotestsrc");
        stats.encoder = QStringLiteral("vp8enc");
        stats.scaling = QStringLiteral("CPU");
        stats.width = 1920;
        stats.height = 1080;
        stats.bytes = 4000;
        stats.frames = 1;
        auto row = stats.sample(100);
        QCOMPARE(row.value("kbps").toDouble(), -1.0);
        QCOMPARE(row.value("width").toInt(), 1920);
        QCOMPARE(row.value("height").toInt(), 1080);
        QCOMPARE(row.value("encoder").toString(), QStringLiteral("vp8enc"));
        stats.bytes += 125000;
        stats.frames += 30;
        stats.packets = 123;
        stats.decrypted = 28;
        stats.dropped = 2;
        row = stats.sample(1100);
        QCOMPARE(row.value("kbps").toDouble(), 1000.0);
        QCOMPARE(row.value("fps").toDouble(), 30.0);
        QCOMPARE(row.value("packets").toULongLong(), 123ULL);
        QCOMPARE(row.value("decrypted").toULongLong(), 28ULL);
        QCOMPARE(row.value("dropped").toULongLong(), 2ULL);
        QCOMPARE(row.size(), 13); // Closed allow-list; no transport/key structures.
        stats.lastMs = -1; // Hidden, then shown: never average over hidden time.
        QCOMPARE(stats.sample(5000).value("fps").toDouble(), -1.0);
    }
};
QTEST_GUILESS_MAIN(StreamInfoStatsTest)
#include "StreamInfoStatsTest.moc"
