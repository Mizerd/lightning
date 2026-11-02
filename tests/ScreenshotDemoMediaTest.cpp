
#include "matrix/MockMatrixClient.h"
#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

class ScreenshotDemoMediaTest : public QObject
{
    Q_OBJECT

    struct Delivered { bool ok = false; QByteArray bytes; QString mime; };
    static Delivered awaitMedia(MockMatrixClient &c, quint64 op)
    {
        Delivered d;
        if (op == 0)
            return d;
        QSignalSpy spy(&c, &MatrixClient::mediaReady);
        // The mock delivers on the event loop; pump until the op arrives.
        for (int i = 0; i < 50 && spy.isEmpty(); ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        for (const auto &args : spy) {
            if (args.at(0).toULongLong() != op)
                continue;
            d.ok = true;
            d.bytes = args.at(3).toByteArray();
            d.mime = args.at(4).toString();
        }
        return d;
    }

private Q_SLOTS:
    void bridgeIsDemoOnly()
    {
        MockMatrixClient plain;
        QVERIFY(!plain.supportsMediaBridge());
        QCOMPARE(plain.fetchMedia(QStringLiteral("coast"), 1), quint64(0));

        MockMatrixClient demo;
        demo.setScreenshotDemoMode(true);
        QVERIFY(demo.supportsMediaBridge());
    }

    void everyImageRowResolvesToFixtureBytes()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        int checked = 0;
        for (const QString &uid : c.demoAccountUserIds()) {
            c.activateDemoAccount(uid);
            for (const RoomInfo &r : c.rooms()) {
                for (const TimelineEvent &e : c.timeline(r.id)) {
                    const bool imageLike = e.type == TimelineEvent::Image
                        || e.type == TimelineEvent::Video
                        || e.type == TimelineEvent::Sticker;
                    if (!imageLike || e.mediaMxcUrl.isEmpty())
                        continue;
                    QVERIFY2(!e.mediaKey.isEmpty(),
                             qUtf8Printable("no mediaKey: " + e.mediaMxcUrl));
                    QVERIFY(e.mediaSourceAvailable);
                    QVERIFY(e.mediaThumbAvailable);
                    QVERIFY2(e.mediaWidth > 0 && e.mediaHeight > 0,
                             qUtf8Printable("no dimensions: " + e.mediaKey));
                    const Delivered thumb = awaitMedia(c, c.fetchMedia(e.mediaKey, 1));
                    QVERIFY2(thumb.ok && !thumb.bytes.isEmpty(),
                             qUtf8Printable("thumb did not resolve: " + e.mediaKey));
                    const Delivered full = awaitMedia(c, c.fetchMedia(e.mediaKey, 0));
                    QVERIFY2(full.ok && !full.bytes.isEmpty(),
                             qUtf8Printable("full did not resolve: " + e.mediaKey));
                    QVERIFY(thumb.mime.startsWith(QStringLiteral("image/")));
                    ++checked;
                }
            }
        }
        QVERIFY2(checked >= 10,
                 qUtf8Printable(QStringLiteral("only %1 media rows checked")
                                    .arg(checked)));
    }

    void gifFixtureIsAGif()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const Delivered d = awaitMedia(c, c.fetchMedia(QStringLiteral("loop"), 0));
        QVERIFY(d.ok);
        QVERIFY(!d.bytes.isEmpty());
        QCOMPARE(d.mime, QStringLiteral("image/gif"));
        QVERIFY(d.bytes.startsWith("GIF"));
    }

    void everyAccountAvatarResolves()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        const QStringList people = { QStringLiteral("alex"), QStringLiteral("taylor"),
            QStringLiteral("nova"), QStringLiteral("maya"), QStringLiteral("jordan"),
            QStringLiteral("sam"), QStringLiteral("aisha"), QStringLiteral("noah"),
            QStringLiteral("priya"), QStringLiteral("leo") };
        for (const QString &p : people) {
            const QString mxc = QStringLiteral("mxc://lightning.example/avatar-") + p;
            const Delivered d = awaitMedia(c, c.fetchMxcThumbnail(mxc, 224, 224));
            QVERIFY2(d.ok && !d.bytes.isEmpty(),
                     qUtf8Printable("avatar did not resolve: " + p));
            QCOMPARE(d.mime, QStringLiteral("image/png"));
            QVERIFY(d.bytes.startsWith("\x89PNG"));
        }
    }

    void unknownKeyReportsUnavailable()
    {
        MockMatrixClient c;
        c.setScreenshotDemoMode(true);
        QCOMPARE(c.fetchMedia(QStringLiteral("does-not-exist"), 1), quint64(0));
        QCOMPARE(c.fetchMxcThumbnail(
                     QStringLiteral("mxc://lightning.example/avatar-nobody"),
                     224, 224),
                 quint64(0));
    }
};

QTEST_GUILESS_MAIN(ScreenshotDemoMediaTest)
#include "ScreenshotDemoMediaTest.moc"
