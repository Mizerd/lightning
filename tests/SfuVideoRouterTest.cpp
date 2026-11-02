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
        QVERIFY(!router.watching(QString()));
    }

    void attachingMakesAStreamWatched()
    {
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), sink.get());
        QVERIFY(router.watching(QStringLiteral("PA_alice")));
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
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), sink.get());
        QVERIFY(router.watching(QStringLiteral("PA_alice")));
        router.attachSink(QStringLiteral("PA_alice"), nullptr);
        QVERIFY2(router.watching(QStringLiteral("PA_alice")),
                 "a null attach evicted the live owner");
        QVERIFY(router.watchedBy(QStringLiteral("PA_alice"), sink.get()));

        router.attachSink(QString(), sink.get());
        QVERIFY(!router.watching(QString()));
    }

    void aDestroyedSinkStopsBeingWatched()
    {
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

        auto other = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_bob"), other.get());
        QSignalSpy otherSpy(other.get(), &QVideoSink::videoFrameChanged);
        router.deliverFrame(QStringLiteral("PA_alice"), frame);
        QCOMPARE(otherSpy.count(), 0);
    }

    void offeredFramesKeepOnlyTheNewestPerKey()
    {
        SfuVideoRouter router;
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(QStringLiteral("PA_alice"), sink.get());
        QSignalSpy spy(sink.get(), &QVideoSink::videoFrameChanged);
        const QVideoFrameFormat format(QSize(16, 16),
                                       QVideoFrameFormat::Format_RGBA8888);
        int deliveriesAsked = 0;
        QVideoFrame last;
        for (int i = 0; i < 100; ++i) {
            QVideoFrame frame(format);
            frame.setStartTime(i);
            last = frame;
            if (router.offerFrame(QStringLiteral("PA_alice"), frame))
                ++deliveriesAsked;
        }
        QCOMPARE(deliveriesAsked, 1);
        QCOMPARE(router.pendingFramesForTest(), 1);
        router.deliverPending(QStringLiteral("PA_alice"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(sink->videoFrame().startTime(), last.startTime());
        QCOMPARE(router.pendingFramesForTest(), 0);
        router.deliverPending(QStringLiteral("PA_alice"));
        QCOMPARE(spy.count(), 1);
        QVERIFY(router.offerFrame(QStringLiteral("PA_alice"), QVideoFrame(format)));
        QVERIFY(router.offerFrame(QStringLiteral("PA_bob"), QVideoFrame(format)));
        QCOMPARE(router.pendingFramesForTest(), 2);
        router.clear();
        QCOMPARE(router.pendingFramesForTest(), 0);
    }

    void clearDropsEverySink()
    {
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
        router.releaseSink(nullptr);
        QVERIFY(!router.watching(QStringLiteral("PA_nobody")));
    }

    void aSupersededSurfaceCannotTearDownItsReplacement()
    {
        SfuVideoRouter router;
        auto oldSurface = std::make_unique<QVideoSink>();
        auto newSurface = std::make_unique<QVideoSink>();
        const QString key = QStringLiteral("TR_share_a");

        router.attachSink(key, oldSurface.get());
        router.attachSink(key, newSurface.get());
        router.releaseSink(oldSurface.get());

        QVERIFY2(router.watching(key), "a dying surface unhooked a live one");
        QVERIFY(router.watchedBy(key, newSurface.get()));

        router.releaseSink(newSurface.get());
        QVERIFY(!router.watching(key));
    }

    void oneSurfaceGivesUpEveryKeyItHoldsAndNobodyElses()
    {
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
