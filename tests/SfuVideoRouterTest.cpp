// Received video routing (2026-08-23): SfuVideoRouter's sink table.
//
// Until this class existed, received video went to a `fakesink` — the
// pipeline decoded every frame and threw it away, so a video call showed
// nothing. What this suite defends is the part that fails SILENTLY:
//
//  * A tile that is destroyed must stop being a destination. The router
//    holds QPointers precisely because a VideoOutput can die between a frame
//    being queued on a GStreamer streaming thread and delivered on the GUI
//    thread, and that window opens on every grid relayout.
//  * `watching()` is what the engine consults BEFORE copying a frame, so a
//    stale "yes" costs a full-frame memcpy per frame for a tile nobody is
//    looking at.
//  * Re-attaching one stream must REPLACE, not accumulate: a tile rebuilt by
//    a relayout would otherwise leave the old sink receiving frames.
#include "calls/SfuVideoRouter.h"

#include <memory>

#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#include <QtTest/QtTest>

class SfuVideoRouterTest : public QObject
{
    Q_OBJECT

private slots:
    void anUnattachedStreamIsNotWatched()
    {
        SfuVideoRouter router;
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
        // Empty is not a stream. Accepting it would make one bad frame
        // attribution route into a shared bucket.
        QVERIFY(!router.watching(QString()));
    }

    void attachingMakesAStreamWatched()
    {
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), sink.get());
        QVERIFY(router.watching(QStringLiteral("PA_alice")));
        // Only that stream: a second participant is still unwatched, so the
        // engine does not start copying their frames too.
        QVERIFY(!router.watching(QStringLiteral("PA_bob")));
    }

    void anEmptyStreamIdIsRefused()
    {
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QString(), sink.get());
        QVERIFY(!router.watching(QString()));
    }

    void aNullSinkDetachesRatherThanRegisteringNothing()
    {
        // A QML VideoOutput whose videoSink is not ready yet passes null.
        // Recording that as an attachment would report the stream as
        // watched, and the engine would copy frames into a hole.
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), sink.get());
        QVERIFY(router.watching(QStringLiteral("PA_alice")));
        router.attachSink(QStringLiteral("PA_alice"), nullptr);
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
    }

    void aDestroyedSinkStopsBeingWatched()
    {
        // THE case the QPointer exists for. A raw pointer would report this
        // stream as watched forever and deliverFrame would dereference freed
        // memory on the next frame.
        SfuVideoRouter router;
        {
            auto sink = std::make_unique<QVideoSink>();
            router.attachSink(QStringLiteral("PA_alice"), sink.get());
            QVERIFY(router.watching(QStringLiteral("PA_alice")));
        }
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
    }

    void deliveringToADestroyedSinkIsSafeAndForgetsIt()
    {
        SfuVideoRouter router;
        {
            auto sink = std::make_unique<QVideoSink>();
            router.attachSink(QStringLiteral("PA_alice"), sink.get());
        }
        // Must not crash, and must drop the entry so the next frame does not
        // pay the lookup again.
        router.deliverFrame(QStringLiteral("PA_alice"), QVideoFrame());
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
    }

    void reattachingReplacesRatherThanAccumulates()
    {
        SfuVideoRouter router;
        auto first = std::make_unique<QVideoSink>();
        auto second = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), first.get());
        router.attachSink(QStringLiteral("PA_alice"), second.get());

        // The frame goes to the CURRENT sink only. Destroying the first one
        // must not make the stream unwatched, which is what would happen if
        // the table had kept it.
        first.reset();
        QVERIFY(router.watching(QStringLiteral("PA_alice")));
    }

    void aDeliveredFrameReachesTheAttachedSink()
    {
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), sink.get());

        QSignalSpy spy(sink.get(), &QVideoSink::videoFrameChanged);
        QVideoFrame frame(QVideoFrameFormat(
            QSize(16, 16), QVideoFrameFormat::Format_RGBA8888));
        router.deliverFrame(QStringLiteral("PA_alice"), frame);
        QCOMPARE(spy.count(), 1);

        // ...and NOT to a different participant's sink. Cross-routing video
        // is the kind of defect that looks like a working call.
        auto other = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_bob"), other.get());
        QSignalSpy otherSpy(other.get(), &QVideoSink::videoFrameChanged);
        router.deliverFrame(QStringLiteral("PA_alice"), frame);
        QCOMPARE(otherSpy.count(), 0);
    }

    void clearDropsEverySink()
    {
        // Teardown. A sink attached for the call that just ended is a live
        // destination for the NEXT call's frames, whose stream ids the SFU
        // assigns afresh.
        SfuVideoRouter router;
        auto a = std::make_unique<QVideoSink>();
        auto b = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), a.get());
        router.attachSink(QStringLiteral("PA_bob"), b.get());
        router.clear();
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
        QVERIFY(!router.watching(QStringLiteral("PA_bob")));
    }

    void detachingAnUnknownStreamIsHarmless()
    {
        SfuVideoRouter router;
        router.detachSink(QStringLiteral("PA_nobody"));
        router.detachSink(QString());
        QVERIFY(!router.watching(QStringLiteral("PA_nobody")));
    }
};

QTEST_MAIN(SfuVideoRouterTest)
#include "SfuVideoRouterTest.moc"
