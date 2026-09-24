// Received video routing: SfuVideoRouter's sink table. Defends the parts that
// fail silently:
//
//  * A destroyed tile stops being a destination. The router holds QPointers
//    because a VideoOutput can die between a frame being queued on a
//    GStreamer streaming thread and delivered on the GUI thread.
//  * `watching()` is consulted before copying a frame, so a stale "yes" costs
//    a full-frame copy for a tile nobody is looking at.
//  * Re-attaching one stream replaces rather than accumulates.
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

    void aNullSinkRegistersNothingAndEvictsNobody()
    {
        // A VideoOutput whose videoSink is not ready passes null. That must
        // not be stored (the stream would read as watched), and must not
        // remove an existing working sink either.
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), sink.get());
        QVERIFY(router.watching(QStringLiteral("PA_alice")));
        router.attachSink(QStringLiteral("PA_alice"), nullptr);
        QVERIFY2(router.watching(QStringLiteral("PA_alice")),
                 "a null attach evicted the live owner");
        QVERIFY(router.watchedBy(QStringLiteral("PA_alice"), sink.get()));

        // And an empty key still registers nothing.
        router.attachSink(QString(), sink.get());
        QVERIFY(!router.watching(QString()));
    }

    void aDestroyedSinkStopsBeingWatched()
    {
        // The case the QPointer exists for: a raw pointer would report the
        // stream as watched forever and dereference freed memory.
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
        // Teardown: a sink from the call that ended must not receive the next
        // call's frames, whose stream ids the SFU assigns afresh.
        SfuVideoRouter router;
        auto a = std::make_unique<QVideoSink>();
        auto b = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), a.get());
        router.attachSink(QStringLiteral("PA_bob"), b.get());
        router.clear();
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
        QVERIFY(!router.watching(QStringLiteral("PA_bob")));
    }

    void releasingASinkThatOwnsNothingIsHarmless()
    {
        SfuVideoRouter router;
        auto stranger = std::make_unique<QVideoSink>();
        router.releaseSink(stranger.get());
        router.releaseSink(nullptr); // a null release is not a wildcard
        QVERIFY(!router.watching(QStringLiteral("PA_nobody")));
    }

    void aSupersededSurfaceCannotTearDownItsReplacement()
    {
        // A detach names the surface it removes, so a replacement built before
        // the old surface is destroyed is not evicted. The order below is
        // production's: Qt builds the replacement synchronously and destroys
        // the old surface on the deferred-delete queue.
        SfuVideoRouter router;
        auto oldSurface = std::make_unique<QVideoSink>();
        auto newSurface = std::make_unique<QVideoSink>();
        const QString key = QStringLiteral("TR_share_a");

        router.attachSink(key, oldSurface.get());
        router.attachSink(key, newSurface.get()); // the replacement claims it
        router.releaseSink(oldSurface.get());     // ...then the old one dies

        QVERIFY2(router.watching(key), "a dying surface unhooked a live one");
        QVERIFY(router.watchedBy(key, newSurface.get()));

        // The real owner's release still works, or every assertion above is
        // vacuous and the table simply never shrinks.
        router.releaseSink(newSurface.get());
        QVERIFY(!router.watching(key));
    }

    void oneSurfaceGivesUpEveryKeyItHoldsAndNobodyElses()
    {
        // A camera tile attaches under BOTH the camera track sid and the
        // participant sid (SfuCallController::attachVideoSink), so a release
        // has to cover every key that sink owns — and no key it does not.
        SfuVideoRouter router;
        auto mine = std::make_unique<QVideoSink>();
        auto theirs = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("TR_cam"), mine.get());
        router.attachSink(QStringLiteral("PA_alice"), mine.get());
        router.attachSink(QStringLiteral("TR_other"), theirs.get());

        router.releaseSink(mine.get());

        QVERIFY(!router.watching(QStringLiteral("TR_cam")));
        QVERIFY(!router.watching(QStringLiteral("PA_alice")));
        QVERIFY(router.watching(QStringLiteral("TR_other")));
    }
};

QTEST_MAIN(SfuVideoRouterTest)
#include "SfuVideoRouterTest.moc"
