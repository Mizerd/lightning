
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"
#include "media/AnimatedImageSniff.h"
#include "media/MediaImageProvider.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QImageReader>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {

QStringList &capturedLines()
{
    static QStringList lines;
    return lines;
}
void captureHandler(QtMsgType, const QMessageLogContext &ctx, const QString &msg)
{
    capturedLines() << (QString::fromUtf8(ctx.category ? ctx.category : "")
                        + QLatin1Char(' ') + msg);
}

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    struct Fetch {
        quint64 opId;
        QString key;
        int kind;
        int width = 0;
        int height = 0;
        int timeoutClass = 0;
    };
    QList<Fetch> fetches;
    QList<quint64> cancels;
    bool rejectFetches = false;

    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int kind,
                       int timeoutClass) override
    {
        if (rejectFetches)
            return 0;
        const quint64 op = nextOp++;
        fetches.append({ op, mediaKey, kind, 0, 0, timeoutClass });
        return op;
    }
    quint64 fetchMxcThumbnail(const QString &mxc, int width, int height) override
    {
        if (rejectFetches)
            return 0;
        const quint64 op = nextOp++;
        fetches.append({ op, mxc, 2, width, height });
        return op;
    }
    void cancelMediaFetch(quint64 opId) override { cancels.append(opId); }

    void succeed(quint64 opId, const QByteArray &bytes,
                 const QString &mime = QStringLiteral("image/png"))
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes,
                          mime, QString());
    }
    void fail(quint64 opId, const QString &category)
    {
        Q_EMIT mediaFailed(opId, QString(), 0, category);
    }

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }
};

const QString kMxc = QStringLiteral("mxc://example.org/avatar1");

QByteArray makeGif(int w, int h, int frames, const QByteArray &extra);

QByteArray makeGif(int w, int h, int frames)
{
    return makeGif(w, h, frames, QByteArray());
}

QByteArray gifComment(const QByteArray &text)
{
    QByteArray b("\x21\xfe", 2);
    for (qsizetype off = 0; off < text.size(); off += 255) {
        const QByteArray part = text.mid(off, 255);
        b.append(char(part.size()));
        b.append(part);
    }
    b.append('\0');
    return b;
}

QByteArray gifAppExtension(const QByteArray &id11, const QByteArray &data)
{
    QByteArray b("\x21\xff\x0b", 3);
    b.append(id11.left(11));
    b.append(char(data.size()));
    b.append(data);
    b.append('\0');
    return b;
}

QByteArray makeGif(int w, int h, int frames, const QByteArray &extra)
{
    QByteArray g("GIF89a");
    const auto le16 = [&g](int v) {
        g.append(char(v & 0xff));
        g.append(char((v >> 8) & 0xff));
    };
    le16(w);
    le16(h);
    g.append(char(0x80));
    g.append('\0');
    g.append('\0');
    g.append(QByteArray("\xff\x00\x00\x00\x00\xff", 6));
    g.append(QByteArray("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19));
    g.append(extra);
    for (int i = 0; i < frames; ++i) {
        g.append(QByteArray("\x21\xf9\x04\x00\x0a\x00\x00\x00", 8));
        g.append(char(0x2c));
        le16(0);
        le16(0);
        le16(1);
        le16(1);
        g.append('\0');
        g.append(char(0x02));
        g.append(QByteArray("\x02\x44\x01\x00", 4));
    }
    g.append(char(0x3b));
    return g;
}

QByteArray makeWebpHeader(int w, int h, bool animated)
{
    QByteArray b("RIFF");
    b.append(QByteArray("\x16\x00\x00\x00", 4));
    b.append("WEBPVP8X");
    b.append(QByteArray("\x0a\x00\x00\x00", 4));
    b.append(char(animated ? 0x02 : 0x00));
    b.append(QByteArray(3, '\0'));
    for (const int v : { w - 1, h - 1 }) {
        b.append(char(v & 0xff));
        b.append(char((v >> 8) & 0xff));
        b.append(char((v >> 16) & 0xff));
    }
    return b;
}

QByteArray realAnimatedWebp()
{
    return QByteArray::fromHex(
        "52494646d201000057454250565038580a00000002000000ff0000ff0000414e494d0600"
        "0000ffffffff0000414e4d46d4000000000000000000ff0000ff0000fa00000256503820"
        "bc0000009011009d012a000100013e31188c44a221a1101400200304b4b770bb588f6e03"
        "f003f000000b55a841769502c976bc5c9c87bed9390f7db2721efb64e43df6c9c87bed93"
        "90f7db2721efb64e43df6c9c87bed9390f7db2721efb64e43df6c9c87bed9390f7db2721"
        "efb64e43df6c9c87bed9390f7db2721efb64e43df6c9c87bed9390f7db2721efb64e43df"
        "6c9c87bed9390f7d6000feffb10afffff6331fb467f9257ffff8331fc198fe0cc7ff09f3"
        "06090e000000000000000000414e4d46ca000000000000000000ff0000ff0000fa000000"
        "56503820b2000000d410009d012a000100013e3112894481010000609696ee176b11edc0"
        "7e0000013d7feacb4affd59695ffab2d2bff565a57feacb4affd59695ffab2d2bff565a5"
        "7feacb4affd59695ffab2d2bff565a57feacb4affd59695ffab2d2bff565a57feacb4aff"
        "d59695ffab2d2bff565a57feacb4affd59695ffab2d2bff565a57feacb4affd59695ffab"
        "2d2bff565a57a000feffe62ce7ffff8599fc2ccfe1667ff0b33ffff855b862193a000000"
        "000000000000");
}

QByteArray riffChunk(const char *fourcc, const QByteArray &payload)
{
    QByteArray b(fourcc, 4);
    const quint32 n = quint32(payload.size());
    for (int i = 0; i < 4; ++i)
        b.append(char((n >> (8 * i)) & 0xff));
    b.append(payload);
    if (n & 1u)
        b.append('\0');
    return b;
}

QByteArray withWebpMetadata(QByteArray webp, const QByteArray &exif,
                            const QByteArray &xmp)
{
    webp.append(riffChunk("EXIF", exif));
    webp.append(riffChunk("XMP ", xmp));
    webp[20] = char(static_cast<unsigned char>(webp.at(20)) | 0x0C);
    const quint32 riff = quint32(webp.size() - 8);
    for (int i = 0; i < 4; ++i)
        webp[4 + i] = char((riff >> (8 * i)) & 0xff);
    return webp;
}

QByteArray pngBytes()
{
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::green);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

QByteArray jpegBytes()
{
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::green);
    QByteArray jpg;
    QBuffer buffer(&jpg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG");
    return jpg;
}

QString writeAnimation(FakeClient &client, MediaBridge &bridge,
                       const QString &mxc)
{
    bridge.avatarSource(mxc, 40);
    client.succeed(client.fetches.last().opId, makeGif(8, 8, 2),
                   QStringLiteral("image/gif"));
    return QUrl(bridge.avatarAnimationSource(mxc, false)).toLocalFile();
}

}

class MediaBridgeTest : public QObject
{
    Q_OBJECT

    static bool waitForPlayableCount(QSignalSpy &spy, int expected)
    {
        constexpr int timeoutMs = 5000;
        QElapsedTimer clock;
        clock.start();
        while (spy.count() < expected && clock.elapsed() < timeoutMs)
            spy.wait(25);
        return spy.count() == expected;
    }

private Q_SLOTS:
    void identicalRequestsAreDeduplicated()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(bridge.mediaSource(QStringLiteral("$ev1"), QStringLiteral("thumb")),
                 QString());
        QCOMPARE(bridge.mediaSource(QStringLiteral("$ev1"), QStringLiteral("thumb")),
                 QString());
        QCOMPARE(client.fetches.size(), 2);
    }

    void allAvatarSizesShareOneCanonicalFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        for (int size : { 30, 34, 48, 56 })
            QCOMPARE(bridge.avatarSource(kMxc, size), QString());
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().width, 224);
        QCOMPARE(client.fetches.first().height, 224);

        client.succeed(client.fetches.first().opId, QByteArray("pixels"));
        QString shared;
        for (int size : { 30, 34, 48, 56 }) {
            const QString source = bridge.avatarSource(kMxc, size);
            QVERIFY(source.startsWith(QStringLiteral("image://lightning-media/")));
            if (shared.isEmpty())
                shared = source;
            else
                QCOMPARE(source, shared);
        }
        QCOMPARE(client.fetches.size(), 1);
    }

    void completedFetchIsServedFromCache()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.first().opId, QByteArray("pixels"));
        QCOMPARE(cached.count(), 1);

        const QString source = bridge.avatarSource(kMxc, 64);
        QVERIFY(source.startsWith(QStringLiteral("image://lightning-media/")));
        QCOMPARE(client.fetches.size(), 1);
        const QString cacheKey = cached.first().at(0).toString();
        QCOMPARE(bridge.cachedBytes(cacheKey), QByteArray("pixels"));
    }

    void cacheHitTraceIsSilentByDefault()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.first().opId, QByteArray("pixels"));

        auto &captured = capturedLines();
        captured.clear();
        QtMessageHandler prev = qInstallMessageHandler(&captureHandler);
        const QString hit = bridge.avatarSource(kMxc, 64);
        bridge.avatarSource(QStringLiteral("mxc://mock.local/second"), 64);
        qInstallMessageHandler(prev);

        QVERIFY(hit.startsWith(QStringLiteral("image://lightning-media/")));
        for (const QString &line : captured) {
            QVERIFY2(!line.contains(QStringLiteral("cache=hit")),
                     qUtf8Printable("unexpected cache=hit storm line: " + line));
            QVERIFY2(!line.contains(QStringLiteral("cache=miss")),
                     qUtf8Printable("a per-request line is back on the default "
                                    "category: " + line));
            QVERIFY2(!line.contains(QStringLiteral("already-pending")),
                     qUtf8Printable("the per-CALLER line is back, and it is "
                                    "unbounded in a list: " + line));
        }
    }

    void aBurstOfMediaWorkIsSummarisedOnceItGoesQuiet()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.avatarSource(kMxc, 64);
        bridge.avatarSource(QStringLiteral("mxc://mock.local/second"), 64);

        auto &captured = capturedLines();
        captured.clear();
        QtMessageHandler prev = qInstallMessageHandler(&captureHandler);
        for (const auto &fetch : client.fetches)
            client.succeed(fetch.opId, QByteArray("pixels"));
        bool sawSummary = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const QString &line : captured) {
                if (line.contains(QStringLiteral("media burst:")))
                    sawSummary = true;
            }
            return sawSummary;
        }()), 5000);
        qInstallMessageHandler(prev);

        for (const QString &line : captured) {
            if (!line.contains(QStringLiteral("media burst:")))
                continue;
            QVERIFY2(!line.contains(QStringLiteral("mxc://")),
                     qUtf8Printable("the summary names a media URI: " + line));
            QVERIFY2(line.contains(QStringLiteral("fetched")),
                     qUtf8Printable("the summary carries no count: " + line));
        }
        QVERIFY(sawSummary);
    }

    void evictionIsBoundedLru()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setCacheLimitBytes(10);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.mediaSource(QStringLiteral("$lru-a"), QStringLiteral("thumb"));
        client.succeed(client.fetches.at(0).opId, QByteArray(8, 'a'));
        bridge.mediaSource(QStringLiteral("$lru-b"), QStringLiteral("thumb"));
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));

        const QString firstKey = cached.at(0).at(0).toString();
        const QString secondKey = cached.at(1).at(0).toString();
        QVERIFY(bridge.cachedBytes(firstKey).isEmpty());
        QCOMPARE(bridge.cachedBytes(secondKey), QByteArray(8, 'b'));
        QVERIFY(bridge.cacheBytesUsed() <= 10);
    }

    void theDefaultByteCacheHoldsThirtyTwoMiB()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        const qint64 chunk = 8 * 1024 * 1024;
        for (int i = 0; i < 5; ++i) {
            bridge.mediaSource(QStringLiteral("$def-%1").arg(i),
                               QStringLiteral("thumb"));
            client.succeed(client.fetches.at(i).opId, QByteArray(chunk, 'a'));
        }
        QCOMPARE(cached.count(), 5);
        QVERIFY(bridge.cachedBytes(cached.at(0).at(0).toString()).isEmpty());
        for (int i = 1; i < 5; ++i)
            QVERIFY(!bridge.cachedBytes(cached.at(i).at(0).toString()).isEmpty());
        QCOMPARE(bridge.cacheBytesUsed(), 4 * chunk);
    }
    void avatarClassEvictionIsBoundedLru()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setAvatarCacheLimitBytes(10);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.at(0).opId, QByteArray(8, 'a'));
        bridge.avatarSource(QStringLiteral("mxc://example.org/avatar2"), 64);
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));

        const QString firstKey = cached.at(0).at(0).toString();
        const QString secondKey = cached.at(1).at(0).toString();
        QVERIFY(bridge.cachedBytes(firstKey).isEmpty());
        QCOMPARE(bridge.cachedBytes(secondKey), QByteArray(8, 'b'));
        QVERIFY(bridge.cacheBytesUsed() <= 10);
    }

    void timelineChurnNeverEvictsAvatars()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setCacheLimitBytes(24);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.at(0).opId, QByteArray(12, 'a'));
        const QString avatarKey = cached.at(0).at(0).toString();

        for (int i = 0; i < 20; ++i) {
            bridge.mediaSource(QStringLiteral("$churn%1").arg(i),
                               QStringLiteral("thumb"));
            client.succeed(client.fetches.last().opId, QByteArray(16, 'x'));
        }

        QCOMPARE(bridge.cachedBytes(avatarKey), QByteArray(12, 'a'));
        QVERIFY(!bridge.avatarSource(kMxc, 64).isEmpty());
        int avatarFetches = 0;
        for (const auto &f : client.fetches) {
            if (f.key == kMxc)
                ++avatarFetches;
        }
        QCOMPARE(avatarFetches, 1);
        QVERIFY(bridge.cacheBytesUsed() <= 24 + 12);
    }

    void staleResultAfterSignOutIsRejected()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        const quint64 preLogoutOp = client.fetches.first().opId;
        client.logout();

        client.succeed(preLogoutOp, QByteArray("secret"));
        QCOMPARE(cached.count(), 0);
        QCOMPARE(bridge.cacheBytesUsed(), 0);
    }

    void avatarUriChangeIsANewRequest()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.first().opId, QByteArray("old"));
        bridge.avatarSource(QStringLiteral("mxc://example.org/avatar2"), 64);
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.at(1).key,
                 QStringLiteral("mxc://example.org/avatar2"));
    }

    void failureIsMarkedAndRetryClearsIt()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.avatarSource(kMxc, 64);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));
        QCOMPARE(failed.count(), 1);
        const QString cacheKey = failed.first().at(0).toString();
        QCOMPARE(bridge.failureCategory(cacheKey), QStringLiteral("network"));
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("network"));
        QVERIFY(bridge.avatarFailureCategory(
                    QStringLiteral("mxc://example.org/other")).isEmpty());

        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("network"));

        bridge.retry(cacheKey);
        QVERIFY(bridge.failureCategory(cacheKey).isEmpty());
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    void missingAvatarDispatchesNothing()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        QCOMPARE(bridge.avatarSource(QString(), 64), QString());
        QCOMPARE(bridge.avatarSource(QStringLiteral("https://not-mxc"), 64),
                 QString());
        QCOMPARE(client.fetches.size(), 0);
    }

    void concurrencyIsBoundedAndQueuePumps()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        for (int i = 0; i < 10; ++i)
            bridge.mediaSource(QStringLiteral("$ev%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8);

        client.succeed(client.fetches.first().opId, QByteArray("x"));
        QCOMPARE(client.fetches.size(), 9);
        client.fail(client.fetches.at(1).opId, QStringLiteral("network"));
        QCOMPARE(client.fetches.size(), 10);
    }

    void transientFailureExpiresAndRedispatches()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setFailureRetryMsForTest(0);

        bridge.avatarSource(kMxc, 64);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));
        QCOMPARE(client.fetches.size(), 1);

        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);

        client.fail(client.fetches.at(1).opId, QStringLiteral("rejected"));
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    void serverErrorIsAFastTransientFailure()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        for (int i = 0; i < 10; ++i)
            bridge.avatarSource(QStringLiteral("mxc://remote.example/a%1").arg(i), 64);
        QCOMPARE(client.fetches.size(), 8);

        for (int i = 0; i < 8; ++i)
            client.fail(client.fetches.at(i).opId, QStringLiteral("server_error"));
        QCOMPARE(client.fetches.size(), 10);
        QCOMPARE(bridge.avatarFailureCategory(
                     QStringLiteral("mxc://remote.example/a0")),
                 QStringLiteral("server_error"));

        QCOMPARE(bridge.avatarSource(QStringLiteral("mxc://remote.example/a0"), 64),
                 QString());
        QCOMPARE(client.fetches.size(), 10);

        bridge.setFailureRetryMsForTest(0);
        bridge.avatarSource(QStringLiteral("mxc://remote.example/a0"), 64);
        QCOMPARE(client.fetches.size(), 11);
    }

    void notFoundIsPermanentForTheSession()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString gone = QStringLiteral("mxc://remote.example/gone");
        bridge.avatarSource(gone, 64);
        QCOMPARE(client.fetches.size(), 1);
        client.fail(client.fetches.first().opId, QStringLiteral("not_found"));
        QCOMPARE(bridge.avatarFailureCategory(gone), QStringLiteral("not_found"));

        bridge.setFailureRetryMsForTest(0);
        bridge.checkInflightTimeouts();
        QCOMPARE(bridge.avatarSource(gone, 64), QString());
        QCOMPARE(client.fetches.size(), 1);
    }

    void breakerRefusalBlocksForItsOwnBackoffAndReportsTheWait()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        const QString mxc = QStringLiteral("mxc://dead.example/a");
        bridge.avatarSource(mxc, 64);
        QCOMPARE(client.fetches.size(), 1);
        client.fail(client.fetches.first().opId,
                    QStringLiteral("server_unreachable:240000"));

        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(),
                 QStringLiteral("server_unreachable"));
        QCOMPARE(bridge.avatarFailureCategory(mxc),
                 QStringLiteral("server_unreachable"));
        const QString key = failed.first().at(0).toString();
        const int seconds = bridge.failureRetrySeconds(key);
        QVERIFY2(seconds > 235 && seconds <= 240,
                 qPrintable(QString::number(seconds)));

        bridge.setFailureRetryMsForTest(0);
        bridge.checkInflightTimeouts();
        QCOMPARE(bridge.avatarSource(mxc, 64), QString());
        QCOMPARE(client.fetches.size(), 1);

        bridge.retry(key);
        bridge.avatarSource(mxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    void onlyABreakerRefusalHasARetryWait()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.avatarSource(QStringLiteral("mxc://ok.example/a"), 64);
        client.fail(client.fetches.first().opId, QStringLiteral("server_error"));
        QCOMPARE(bridge.failureRetrySeconds(failed.first().at(0).toString()), 0);

        bridge.avatarSource(QStringLiteral("mxc://ok.example/b"), 64);
        client.fail(client.fetches.last().opId,
                    QStringLiteral("server_unreachable:garbage"));
        QCOMPARE(failed.last().at(1).toString(),
                 QStringLiteral("server_unreachable"));
        const int fallback =
            bridge.failureRetrySeconds(failed.last().at(0).toString());
        QVERIFY2(fallback > 0 && fallback <= 60,
                 qPrintable(QString::number(fallback)));
    }

    void unavailableDispatchIsTransientAndRecovers()
    {
        FakeClient client;
        client.rejectFetches = true;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy retryable(&bridge, &MediaBridge::mediaRetryable);

        bridge.avatarSource(kMxc, 64);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(),
                 QStringLiteral("unavailable"));
        QCOMPARE(bridge.avatarFailureCategory(kMxc),
                 QStringLiteral("unavailable"));

        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(client.fetches.size(), 0);

        client.rejectFetches = false;
        bridge.setFailureRetryMsForTest(0);
        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 1);
        QVERIFY(retryable.first().at(0).toString()
                    .endsWith(QLatin1Char(':') + kMxc));
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 1);
        client.succeed(client.fetches.first().opId, QByteArray("pixels"));
        QVERIFY(!bridge.avatarSource(kMxc, 64).isEmpty());
    }

    void retryAvatarAsksForOneKeyAndNeverForAnOlderAccountsAvatar()
    {
        const QString oldMxc = QStringLiteral("mxc://old.example/avatar");
        FakeClient client;
        client.rejectFetches = true;
        MediaBridge bridge;
        bridge.setClient(&client);
        QStringList reRequested;
        QObject::connect(&bridge, &MediaBridge::mediaRetryable, &bridge,
                         [&](const QString &cacheKey) {
                             reRequested.append(cacheKey);
                         });

        bridge.avatarSource(oldMxc, 64);
        QCOMPARE(bridge.avatarFailureCategory(oldMxc),
                 QStringLiteral("unavailable"));
        client.rejectFetches = false;
        bridge.clear();
        bridge.clear();
        bridge.clear();
        QVERIFY(reRequested.isEmpty());

        bridge.retryAvatar(kMxc);
        QCOMPARE(reRequested.size(), 1);
        QVERIFY(reRequested.first().endsWith(QLatin1Char(':') + kMxc));
        QVERIFY(!reRequested.first().contains(oldMxc));
        bridge.retryAvatar(QString());
        QCOMPARE(reRequested.size(), 1);
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 1);
    }

    void sweepEmitsMediaRetryableForExpiredTransientMarks()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy retryable(&bridge, &MediaBridge::mediaRetryable);

        bridge.avatarSource(kMxc, 64);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));

        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 0);
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("network"));

        bridge.setFailureRetryMsForTest(1);
        QTest::qWait(5);
        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 1);
        QVERIFY(retryable.first().at(0).toString()
                    .endsWith(QLatin1Char(':') + kMxc));
        QVERIFY(bridge.avatarFailureCategory(kMxc).isEmpty());
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);

        client.fail(client.fetches.at(1).opId, QStringLiteral("rejected"));
        QTest::qWait(5);
        bridge.checkInflightTimeouts();
        QCOMPARE(retryable.count(), 1);
        QCOMPARE(bridge.avatarFailureCategory(kMxc), QStringLiteral("rejected"));
        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    void revisionChangesExactlyOnByteInsert()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setAvatarCacheLimitBytes(10);

        bridge.avatarSource(kMxc, 64);
        client.succeed(client.fetches.at(0).opId, QByteArray(6, 'a'));
        const QString url1 = bridge.avatarSource(kMxc, 64);
        QVERIFY(url1.contains(QStringLiteral("?r=")));
        QCOMPARE(bridge.avatarSource(kMxc, 64), url1);

        bridge.avatarSource(QStringLiteral("mxc://example.org/avatar2"), 64);
        client.succeed(client.fetches.at(1).opId, QByteArray(8, 'b'));
        QCOMPARE(bridge.avatarSource(kMxc, 64), QString());
        QCOMPARE(client.fetches.size(), 3);
        client.succeed(client.fetches.at(2).opId, QByteArray(6, 'a'));
        const QString url2 = bridge.avatarSource(kMxc, 64);
        QVERIFY(!url2.isEmpty());
        QVERIFY2(url2 != url1, "re-cached bytes must yield a NEW source string");
    }

    void stuckRequestTimesOutAndReclaimsSlot()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$stuck%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8);
        QCOMPARE(bridge.inflightCountForTest(), 8);

        bridge.mediaSource(QStringLiteral("$fresh"), QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8);
        QCOMPARE(bridge.queuedCountForTest(), 1);

        bridge.setInflightTimeoutMsForTest(0);
        bridge.checkInflightTimeouts();

        QCOMPARE(failed.count(), 8);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("timeout"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        QCOMPARE(bridge.queuedCountForTest(), 0);
        QCOMPARE(client.fetches.size(), 9);
        QCOMPARE(bridge.healthSnapshot().value(QStringLiteral("timedOut"))
                     .toLongLong(),
                 8);
    }

    void timedOutRequestRedispatchesAfterTransientExpiry()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setInflightTimeoutMsForTest(0);
        bridge.setFailureRetryMsForTest(0);

        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 1);
        bridge.checkInflightTimeouts();
        QCOMPARE(bridge.inflightCountForTest(), 0);

        bridge.avatarSource(kMxc, 64);
        QCOMPARE(client.fetches.size(), 2);
    }

    void saturationDrainsToZeroWithBoundedMemory()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setCacheLimitBytes(64);

        constexpr int kN = 200;
        for (int i = 0; i < kN; ++i)
            bridge.mediaSource(QStringLiteral("$soak%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(bridge.inflightCountForTest(), 8);
        QCOMPARE(bridge.queuedCountForTest(), kN - 8);

        int resolved = 0;
        int guard = 0;
        while (resolved < client.fetches.size() && guard++ < kN * 8) {
            const quint64 op = client.fetches.at(resolved).opId;
            if (resolved % 5 == 0)
                client.fail(op, QStringLiteral("network"));
            else
                client.succeed(op, QByteArray("x"));
            QVERIFY(bridge.inflightCountForTest() <= 8);
            QVERIFY(bridge.cacheBytesUsed() <= 64);
            ++resolved;
        }

        QCOMPARE(client.fetches.size(), kN);
        QCOMPARE(bridge.inflightCountForTest(), 0);
        QCOMPARE(bridge.queuedCountForTest(), 0);
        const QVariantMap snap = bridge.healthSnapshot();
        QCOMPARE(snap.value(QStringLiteral("completed")).toLongLong()
                     + snap.value(QStringLiteral("failed")).toLongLong(),
                 static_cast<qint64>(kN));
        QVERIFY(bridge.cacheBytesUsed() <= 64);

        bridge.mediaSource(QStringLiteral("$after"), QStringLiteral("thumb"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
    }

    void animatedGifUsesValidatedLocalFileAndCleansOnLogout()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QCOMPARE(bridge.animatedSource(QStringLiteral("$gif")), QString());
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.first().opId, gif, QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 1);
        const QString source = bridge.animatedSource(QStringLiteral("$gif"));
        QVERIFY(source.startsWith(QStringLiteral("file://")));
        const QString path = QUrl(source).toLocalFile();
        QVERIFY(QFileInfo::exists(path));
        client.logout();
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(bridge.animatedSource(QStringLiteral("send-queue.localhost/$txn")).isEmpty());
    }

    void fakeAndOversizedGifsNeverReachAnimatedImage()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.animatedSource(QStringLiteral("$fake"));
        client.succeed(client.fetches.last().opId, QByteArray("<html>not gif</html>"),
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 1);

        bridge.animatedSource(QStringLiteral("$large"));
        QByteArray large("GIF89a");
        large.resize(20 * 1024 * 1024 + 1, '\0');
        client.succeed(client.fetches.last().opId, large, QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 2);
    }

    void animationIsDecidedByTheBytesNotTheDeclaredMimetype()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QCOMPARE(bridge.animatedSource(QStringLiteral("$sticker")), QString());
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.first().opId, gif, QString());
        QCOMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(bridge.animatedSource(QStringLiteral("$sticker"))
                    .startsWith(QStringLiteral("file://")));
    }

    void aLabelSayingGifCannotMakeNonGifBytesAnimatable()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        bridge.animatedSource(QStringLiteral("$png"));
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png,
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
    }

    void aSpeculativeAnimationAskIsAnsweredWithSilenceNotAFailure()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.animatedSource(QStringLiteral("$still"),true);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(cached.count(), 1);
        QCOMPARE(bridge.failureCategory(QStringLiteral("full:$still")), QString());
    }

    void oneDemandingAskerRestoresTheFailureObligation()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.animatedSource(QStringLiteral("$mixed"),true);
        bridge.animatedSource(QStringLiteral("$mixed"),false);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(failed.count(), 1);
    }

    void animatedExtensionIsMagicOnlyAndNarrow()
    {
        const auto ext = [](const QByteArray &b) {
            return MediaBridge::animatedExtensionFor(b);
        };
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        QCOMPARE(ext(gif), QStringLiteral("gif"));
        QByteArray gif87("GIF87a");
        gif87.append(QByteArray(64, '\0'));
        QCOMPARE(ext(gif87), QStringLiteral("gif"));

        const auto webp = [](char flags, const char *fourcc) {
            QByteArray b("RIFF", 4);
            b.append(QByteArray(4, '\0'));
            b.append("WEBP", 4);
            b.append(fourcc, 4);
            b.append(QByteArray(4, '\0'));
            b.append(flags);
            b.append(QByteArray(32, '\0'));
            return b;
        };
        QCOMPARE(ext(webp('\x02', "VP8X")), QStringLiteral("webp"));
        QVERIFY(ext(webp('\x10', "VP8X")).isEmpty());
        QVERIFY(ext(webp('\x00', "VP8 ")).isEmpty());

        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        QVERIFY(ext(png).isEmpty());
        QVERIFY(ext(QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"))
                    .isEmpty());
        QVERIFY(ext(QByteArrayLiteral("GIF")).isEmpty());
        QVERIFY(ext(QByteArray()).isEmpty());
    }

    void stickerPickerAsksForTheOriginalBytesNotAServerThumbnail()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QCOMPARE(bridge.mxcAnimatedSource(kMxc), QString());
        QCOMPARE(client.fetches.size(), 1);
        const FakeClient::Fetch fetch = client.fetches.first();
        QCOMPARE(fetch.key, kMxc);
        QCOMPARE(fetch.width, 0);
        QCOMPARE(fetch.height, 0);

        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        client.succeed(fetch.opId, gif, QString());
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(),
                 QStringLiteral("mxcanim:") + kMxc);
        const QString source = bridge.mxcAnimatedSource(kMxc);
        QVERIFY(source.startsWith(QStringLiteral("file://")));
        const QString path = QUrl(source).toLocalFile();
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(QFileInfo(path).permissions()
                     & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                        | QFileDevice::ReadOther | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        client.logout();
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(bridge.mxcAnimatedSource(QStringLiteral("https://evil.example/x.gif"))
                    .isEmpty());
    }

    void theStillTileAndTheAnimatedPickerTileAreSeparateFetches()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        bridge.mxcImageSource(kMxc, 160);
        bridge.mxcAnimatedSource(kMxc);
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.at(0).width, 160);
        QCOMPARE(client.fetches.at(1).width, 0);

        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.at(0).opId, png, QStringLiteral("image/png"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(bridge.mxcAnimatedSource(kMxc), QString());

        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.at(1).opId, gif, QString());
        QCOMPARE(ready.count(), 1);
        QVERIFY(bridge.mxcAnimatedSource(kMxc).startsWith(QStringLiteral("file://")));
    }

    void aNonAnimatedPickerTileIsAnsweredWithSilenceNotAFailure()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.mxcAnimatedSource(kMxc);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(bridge.failureCategory(QStringLiteral("mxcanim:") + kMxc),
                 QString());
    }

    void aPickerTileRefusesMarkupAndGzipBeforeAnyDecode()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.mxcAnimatedSource(kMxc);
        client.succeed(client.fetches.last().opId,
                       QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"),
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(cached.count(), 0);
        QVERIFY(bridge.mxcAnimatedSource(kMxc).isEmpty());

        const QString svgz = QStringLiteral("mxc://example.org/svgz");
        bridge.mxcAnimatedSource(svgz);
        QByteArray gz("\x1F\x8B\x08", 3);
        gz.append(QByteArray(64, '\0'));
        client.succeed(client.fetches.last().opId, gz, QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(cached.count(), 0);
    }

    void anAvContainerNeverEntersThePickerImagePath()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        QSignalSpy learned(&bridge, &MediaBridge::playableSizeLearned);
        bridge.mxcAnimatedSource(kMxc);
        QByteArray mp4(4, '\0');
        mp4.append("ftypisom", 8);
        mp4.append(QByteArray(32, '\0'));
        client.succeed(client.fetches.last().opId, mp4, QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(cached.count(), 0);
        QCOMPARE(learned.count(), 0);
    }

    void clientPreviewGifUsesTheSameControlledFilePath()
    {
        MediaBridge bridge;
        QByteArray gif("GIF89a"); gif.append(QByteArray(32, '\0'));
        const QString data = QStringLiteral("data:image/gif;base64,")
            + QString::fromLatin1(gif.toBase64());
        const QString source = bridge.previewAnimatedSource(data, QStringLiteral("image/gif"));
        QVERIFY(source.startsWith(QStringLiteral("file://")));
        QVERIFY(QFileInfo::exists(QUrl(source).toLocalFile()));
        QVERIFY(bridge.previewAnimatedSource(QStringLiteral("data:text/html;base64,QQ=="),
                                             QStringLiteral("image/gif")).isEmpty());
    }

    void clientPreviewStaticImageUsesControlledProviderSource()
    {
        MediaBridge bridge;
        QByteArray png(24, '\0');
        png.replace(0, 8, QByteArray("\x89PNG\r\n\x1a\n", 8));
        const QString data = QStringLiteral("data:image/png;base64,")
            + QString::fromLatin1(png.toBase64());
        const QString source = bridge.previewImageSource(data, QStringLiteral("image/png"));
        QVERIFY(source.startsWith(QStringLiteral("image://lightning-media/")));
        QVERIFY(!source.startsWith(QStringLiteral("http")));
        QVERIFY(!source.startsWith(QStringLiteral("file:")));
        QVERIFY(bridge.previewImageSource(
                    QStringLiteral("data:image/png;base64,PGh0bWw+"),
                    QStringLiteral("image/png")).isEmpty());
        QVERIFY(bridge.previewImageSource(
                    QStringLiteral("data:image/svg+xml;base64,PHN2Zz4="),
                    QStringLiteral("image/svg+xml")).isEmpty());
        QByteArray oversized("GIF89a");
        oversized.resize(5 * 1024 * 1024 + 1, '\0');
        QVERIFY(bridge.previewImageSource(
                    QStringLiteral("data:image/gif;base64,")
                        + QString::fromLatin1(oversized.toBase64()),
                    QStringLiteral("image/gif")).isEmpty());
    }

    void playableSniffingIsFailClosed()
    {
        auto ext = [](const QByteArray &bytes, const char *mime) {
            return MediaBridge::playableExtensionFor(
                bytes, QString::fromLatin1(mime));
        };
        QByteArray mp4 = QByteArrayLiteral("\x00\x00\x00\x18""ftypisom");
        mp4.resize(64, '\0');
        QCOMPARE(ext(mp4, "video/mp4"), QStringLiteral("mp4"));
        QCOMPARE(ext(mp4, "audio/mp4"), QStringLiteral("m4a"));
        QByteArray webm = QByteArrayLiteral("\x1A\x45\xDF\xA3");
        webm.resize(64, '\0');
        QCOMPARE(ext(webm, "video/webm"), QStringLiteral("webm"));
        QCOMPARE(ext(webm, "video/x-matroska"), QStringLiteral("mkv"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(64, '\0');
        QCOMPARE(ext(ogg, "audio/ogg"), QStringLiteral("ogg"));
        QByteArray wav = QByteArrayLiteral("RIFF\x24\x00\x00\x00WAVE");
        wav.resize(64, '\0');
        QCOMPARE(ext(wav, "audio/wav"), QStringLiteral("wav"));
        QByteArray mp3 = QByteArrayLiteral("ID3\x04");
        mp3.resize(64, '\0');
        QCOMPARE(ext(mp3, "audio/mpeg"), QStringLiteral("mp3"));
        QByteArray flac = QByteArrayLiteral("fLaC");
        flac.resize(64, '\0');
        QCOMPARE(ext(flac, "audio/flac"), QStringLiteral("flac"));
        QByteArray sync = QByteArrayLiteral("\xFF\xFB\x90\x00");
        sync.resize(64, '\0');
        QCOMPARE(ext(sync, "audio/mpeg"), QStringLiteral("mp3"));
        QVERIFY(ext(sync, "").isEmpty());
        QByteArray adts = QByteArrayLiteral("\xFF\xF1\x50\x80");
        adts.resize(64, '\0');
        QCOMPARE(ext(adts, "audio/aac"), QStringLiteral("aac"));
        QVERIFY(ext(adts, "audio/mpeg").isEmpty());
        QByteArray junk(64, 'x');
        QVERIFY(ext(junk, "video/mp4").isEmpty());
        QByteArray gif = QByteArrayLiteral("GIF89a");
        gif.resize(64, '\0');
        QVERIFY(ext(gif, "image/gif").isEmpty());
        QVERIFY(ext(QByteArrayLiteral("sh"), "video/mp4").isEmpty());
    }

    void playableSourceMaterializesValidatedFile()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        QCOMPARE(bridge.playableSource(QStringLiteral("$vid")), QString());
        QCOMPARE(client.fetches.size(), 1);
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(2048, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QVERIFY(waitForPlayableCount(ready, 1));
        const QString url = bridge.playableSource(QStringLiteral("$vid"));
        QVERIFY(url.startsWith(QStringLiteral("file:")));
        QVERIFY(url.endsWith(QStringLiteral(".ogg")));
        const QString path = QUrl(url).toLocalFile();
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!QFileInfo(path).fileName().contains(QStringLiteral("$vid")));
        const auto perms = QFileInfo(path).permissions();
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));

        QCOMPARE(bridge.playableSource(QStringLiteral("$bad")), QString());
        QCOMPARE(client.fetches.size(), 2);
        client.succeed(client.fetches.at(1).opId, QByteArray(2048, 'x'),
                       QStringLiteral("video/mp4"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(50);
        QCOMPARE(ready.count(), 1);
        bool sawRejected = false;
        for (const auto &args : failed) {
            if (args.at(0).toString() == QStringLiteral("full:$bad")
                && args.at(1).toString() == QStringLiteral("rejected"))
                sawRejected = true;
        }
        QVERIFY(sawRejected);

        bridge.clear();
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(bridge.playableSource(QStringLiteral("$vid")), QString());
        QCOMPARE(client.fetches.size(), 3);
    }

    void largePlayablePayloadSkipsRamCache()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);

        QCOMPARE(bridge.playableSource(QStringLiteral("$big")), QString());
        QByteArray big = QByteArrayLiteral("OggS");
        big.resize(9 * 1024 * 1024, '\2');
        client.succeed(client.fetches.at(0).opId, big,
                       QStringLiteral("audio/ogg"));
        QVERIFY(waitForPlayableCount(ready, 1));
        QVERIFY(bridge.cacheBytesUsed() < 1024 * 1024);
        QVERIFY(!bridge.playableSource(QStringLiteral("$big")).isEmpty());

        QCOMPARE(bridge.playableSource(QStringLiteral("$small")), QString());
        QByteArray small = QByteArrayLiteral("OggS");
        small.resize(4096, '\3');
        client.succeed(client.fetches.at(1).opId, small,
                       QStringLiteral("audio/ogg"));
        QVERIFY(waitForPlayableCount(ready, 2));
        QVERIFY(bridge.cacheBytesUsed() >= small.size());
    }

    void aFetchedPayloadIsNotWrittenByTheThreadThatReceivesIt()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$vid"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(2 * 1024 * 1024, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        QCOMPARE(ready.count(), 0);
        QVERIFY(bridge.playableSource(QStringLiteral("$vid")).isEmpty());
        QCOMPARE(client.fetches.size(), 1);
        QVERIFY(waitForPlayableCount(ready, 1));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QVERIFY(!bridge.playableSource(QStringLiteral("$vid")).isEmpty());
    }

    void twoClaimantsShareOneWriteAndBothAreServiced()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.prefetchPlayable(QStringLiteral("$dual"), 2 * 1024 * 1024);
        QCOMPARE(client.fetches.size(), 1);
        QByteArray mp4(2 * 1024 * 1024, 'x');
        mp4.replace(4, 4, "ftyp");
        client.succeed(client.fetches.at(0).opId, mp4,
                       QStringLiteral("video/mp4"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        QVERIFY(bridge.playableSource(QStringLiteral("$dual")).isEmpty());
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        QCOMPARE(client.fetches.size(), 1);
        QVERIFY(waitForPlayableCount(ready, 1));
        QTest::qWait(50);
        QCOMPARE(ready.count(), 1);
        QVERIFY(!bridge.playableSource(QStringLiteral("$dual")).isEmpty());
    }

    void aWriteStartedBeforeSignOutPublishesNothing()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$vid"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(8 * 1024 * 1024, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        bridge.clear();
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(300);
        QCOMPARE(ready.count(), 0);
        QVERIFY(bridge.playableSource(QStringLiteral("$vid")).isEmpty());
        QCOMPARE(client.fetches.size(), 2);
    }

    void cancelDuringAWriteAbandonsIt()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$vid"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(9 * 1024 * 1024, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$vid"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(300);
        QCOMPARE(ready.count(), 0);
        QVERIFY(bridge.playableSource(QStringLiteral("$vid")).isEmpty());
        QCOMPARE(client.fetches.size(), 2);
    }

    void cancelPrefetchDropsAPurePrefetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString key = QStringLiteral("$ahead");
        QVERIFY(!bridge.hasPrefetch(key));
        bridge.prefetchPlayable(key, 1024 * 1024);
        QVERIFY(bridge.hasPrefetch(key));
        QCOMPARE(client.fetches.size(), 1);
        bridge.cancelPrefetch(key);
        QVERIFY(!bridge.hasPrefetch(key));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(client.cancels.at(0), client.fetches.at(0).opId);
        QVERIFY(bridge.playableSource(key).isEmpty());
        QCOMPARE(client.fetches.size(), 2);
    }

    void cancelPrefetchNeverDropsAFetchAPlayWants()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString key = QStringLiteral("$played");
        bridge.prefetchPlayable(key, 1024 * 1024);
        QVERIFY(bridge.playableSource(key).isEmpty());
        QCOMPARE(client.fetches.size(), 1);
        bridge.cancelPrefetch(key);
        QCOMPARE(client.cancels.size(), 0);
        bridge.cancelPlayable(key);
        QCOMPARE(client.cancels.size(), 1);
    }

    void cancelPrefetchNeverDropsAnAnimatedConsumersFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString key = QStringLiteral("$sharedgif");
        bridge.prefetchPlayable(key, 1024 * 1024);
        QVERIFY(bridge.animatedSource(key).isEmpty());
        QCOMPARE(client.fetches.size(), 1);
        bridge.cancelPrefetch(key);
        QCOMPARE(client.cancels.size(), 0);
    }

    void cancelPrefetchIgnoresAnExplicitPlayAndAnUnknownKey()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QVERIFY(bridge.playableSource(QStringLiteral("$explicit")).isEmpty());
        bridge.cancelPrefetch(QStringLiteral("$explicit"));
        bridge.cancelPrefetch(QStringLiteral("$never-seen"));
        bridge.cancelPrefetch(QString());
        QCOMPARE(client.cancels.size(), 0);
        QVERIFY(!bridge.hasPrefetch(QStringLiteral("$explicit")));
    }

    void anOverBoundPayloadIsRefusedBeforeAnyWriteStarts()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setPlayableCapsForTest(4, 1024);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        bridge.playableSource(QStringLiteral("$vid"));
        QByteArray ogg = QByteArrayLiteral("OggS");
        ogg.resize(4096, '\1');
        client.succeed(client.fetches.at(0).opId, ogg,
                       QStringLiteral("audio/ogg"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(50);
        QCOMPARE(ready.count(), 0);
        bool sawRejected = false;
        for (const auto &args : failed) {
            if (args.at(0).toString() == QStringLiteral("full:$vid")
                && args.at(1).toString() == QStringLiteral("rejected"))
                sawRejected = true;
        }
        QVERIFY(sawRejected);
    }

    void dispatchCarriesTimeoutClasses()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        bridge.mediaSource(QStringLiteral("$img"), QStringLiteral("full"));
        QCOMPARE(client.fetches.at(0).timeoutClass, 0);

        bridge.playableSource(QStringLiteral("$vid"));
        QCOMPARE(client.fetches.at(1).timeoutClass, 1);

        bridge.saveAs(QStringLiteral("$file"),
                      QUrl::fromLocalFile(QStringLiteral("/tmp/x")));
        QCOMPARE(client.fetches.at(2).timeoutClass, 2);
    }

    void duplicateTerminalIsCountedStale()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.avatarSource(kMxc, 64);
        const quint64 op = client.fetches.first().opId;
        client.succeed(op, QByteArray("pixels"));
        client.succeed(op, QByteArray("pixels-again"));

        QCOMPARE(cached.count(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("droppedStale")).toLongLong(),
                 1);
    }

    void lateSuccessAfterWatchdogIsDiscarded()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.avatarSource(kMxc, 64);
        const quint64 op = client.fetches.first().opId;
        bridge.setInflightTimeoutMsForTest(0);
        bridge.checkInflightTimeouts();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("timeout"));
        QCOMPARE(bridge.inflightCountForTest(), 0);

        client.succeed(op, QByteArray("late"));
        QCOMPARE(cached.count(), 0);
        QCOMPARE(bridge.cacheBytesUsed(), 0);
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("droppedStale")).toLongLong(),
                 1);
    }

    void fetchFullForStarDispatchesAtSaveTimeoutClassAndDeliversBytes()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);

        bridge.fetchFullForStar(QStringLiteral("$gif"));
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().timeoutClass, 2);
        QCOMPARE(starred.count(), 0);

        client.succeed(client.fetches.first().opId, QByteArray("GIF89a..."));
        QCOMPARE(starred.count(), 1);
        QVERIFY(starred.first().at(1).toBool());
        QCOMPARE(starred.first().at(2).toByteArray(), QByteArray("GIF89a..."));
        QCOMPARE(bridge.cacheBytesUsed(), 0);
    }

    void fetchFullForStarServesFromCacheWithoutDispatching()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, QByteArray("cached-bytes"));
        QCOMPARE(client.fetches.size(), 1);

        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);
        bridge.fetchFullForStar(QStringLiteral("$gif"));
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(starred.count(), 1);
        QVERIFY(starred.first().at(1).toBool());
        QCOMPARE(starred.first().at(2).toByteArray(), QByteArray("cached-bytes"));
    }

    void fetchFullForStarDedupsInFlightRequestsForSameMediaKey()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);

        bridge.fetchFullForStar(QStringLiteral("$gif"));
        bridge.fetchFullForStar(QStringLiteral("$gif"));
        QCOMPARE(client.fetches.size(), 1);

        bridge.fetchFullForStar(QStringLiteral("$other"));
        QCOMPARE(client.fetches.size(), 2);

        client.succeed(client.fetches.at(0).opId, QByteArray("a"));
        client.succeed(client.fetches.at(1).opId, QByteArray("b"));
        QCOMPARE(starred.count(), 2);
    }

    void starDispatchFailureNeverEmitsMediaFetchFailed()
    {
        FakeClient client;
        client.rejectFetches = true;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy starred(&bridge, &MediaBridge::mediaBytesForStar);
        QSignalSpy fetchFailed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.fetchFullForStar(QStringLiteral("$gif"));

        QCOMPARE(starred.count(), 1);
        QVERIFY(!starred.first().at(1).toBool());
        QCOMPARE(fetchFailed.count(), 0);
    }

    void ordinaryFetchNotDedupedAgainstInFlightStarRequest()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.fetchFullForStar(QStringLiteral("$gif"));
        const QString source =
            bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        QCOMPARE(source, QString());
        QCOMPARE(client.fetches.size(), 2);

        client.succeed(client.fetches.at(1).opId, QByteArray("real-bytes"));
        QCOMPARE(cached.count(), 1);
    }

    void cachedFullContentHashMatchesSha256OfCachedFullPayloadWithoutDispatching()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        const QByteArray bytes("GIF89a-cached-full-bytes");
        client.succeed(client.fetches.first().opId, bytes);
        QCOMPARE(client.fetches.size(), 1);

        const QString expected = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")), expected);
        QCOMPARE(client.fetches.size(), 1);
    }

    void cachedFullContentHashIsEmptyUnlessTheFullPayloadItselfIsCached()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);

        QVERIFY(bridge.cachedFullContentHash(QString()).isEmpty());
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$never-fetched")).isEmpty());

        bridge.mediaSource(QStringLiteral("$thumb-only"), QStringLiteral("thumb"));
        client.succeed(client.fetches.first().opId, QByteArray("thumbnail-bytes"));
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$thumb-only")).isEmpty());
    }

    void cachedFullContentHashIsMemoizedPerCacheKey()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, QByteArray(4096, 'x'));

        for (int i = 0; i < 8; ++i)
            bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(1));
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")),
                 QString::fromLatin1(QCryptographicHash::hash(
                     QByteArray(4096, 'x'), QCryptographicHash::Sha256).toHex()));

        bridge.setCacheLimitBytes(1);
        bridge.mediaSource(QStringLiteral("$other"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'z'));
        QVERIFY(bridge.cachedFullContentHash(QStringLiteral("$gif")).isEmpty());
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(1));

        bridge.setCacheLimitBytes(64 * 1024 * 1024);
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'y'));
        for (int i = 0; i < 3; ++i)
            bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(2));
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$gif")),
                 QString::fromLatin1(QCryptographicHash::hash(
                     QByteArray(4096, 'y'), QCryptographicHash::Sha256).toHex()));

        bridge.clear();
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, QByteArray(4096, 'y'));
        bridge.cachedFullContentHash(QStringLiteral("$gif"));
        QCOMPARE(bridge.healthSnapshot()
                     .value(QStringLiteral("contentHashComputed")).toLongLong(),
                 qint64(3));

        const QByteArray first(2048, 'p');
        const QByteArray second(2048, 'q');
        bridge.mediaSource(QStringLiteral("$once"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, first);
        const QString firstHex =
            bridge.cachedFullContentHash(QStringLiteral("$once"));
        QCOMPARE(firstHex, QString::fromLatin1(QCryptographicHash::hash(
                     first, QCryptographicHash::Sha256).toHex()));

        bridge.clear();
        bridge.mediaSource(QStringLiteral("$once"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, second);
        QCOMPARE(bridge.cachedFullContentHash(QStringLiteral("$once")),
                 QString::fromLatin1(QCryptographicHash::hash(
                     second, QCryptographicHash::Sha256).toHex()));
    }

    void playbackClassJumpsQueueAheadOfSpeculative()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 8);
        bridge.animatedSource(QStringLiteral("$gif1"));
        bridge.animatedSource(QStringLiteral("$gif2"));
        bridge.playableSource(QStringLiteral("$song"));
        QCOMPARE(bridge.queuedCountForTest(), 3);
        client.succeed(client.fetches.at(0).opId, QByteArray("img"));
        QCOMPARE(client.fetches.size(), 9);
        QCOMPARE(client.fetches.last().key, QStringLiteral("$song"));
        QCOMPARE(client.fetches.last().timeoutClass, 1);
    }

    void heavySlotsLeaveHeadroomForInteractive()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.animatedSource(QStringLiteral("$gif%1").arg(i));
        QCOMPARE(client.fetches.size(), 6);
        QCOMPARE(bridge.queuedCountForTest(), 2);
        bridge.avatarSource(kMxc, 48);
        QCOMPARE(client.fetches.size(), 7);
        QCOMPARE(client.fetches.last().key, kMxc);
        client.succeed(client.fetches.at(0).opId,
                       QByteArray("GIF89a") + QByteArray(16, 'g'),
                       QStringLiteral("image/gif"));
        QCOMPARE(client.fetches.size(), 8);
    }

    void starvedSpeculativeEventuallyDispatches()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setStarvationMsForTest(0);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        bridge.animatedSource(QStringLiteral("$gif"));
        bridge.mediaSource(QStringLiteral("$late"), QStringLiteral("thumb"));
        QCOMPARE(bridge.queuedCountForTest(), 2);
        client.succeed(client.fetches.at(0).opId, QByteArray("img"));
        QCOMPARE(client.fetches.last().key, QStringLiteral("$gif"));
    }

    void pinnedPlayableSurvivesEviction()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setPlayableCapsForTest(2, 64 * 1024 * 1024);
        const auto flac = [](char fill) {
            return QByteArray("fLaC") + QByteArray(9 * 1024 * 1024, fill);
        };
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        int materialized = 0;

        bridge.playableSource(QStringLiteral("$a"));
        client.succeed(client.fetches.at(0).opId, flac('a'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        const QString urlA = bridge.playableSource(QStringLiteral("$a"));
        QVERIFY(urlA.startsWith(QLatin1String("file://")));
        bridge.pinPlayable(QStringLiteral("$a"));

        bridge.playableSource(QStringLiteral("$b"));
        client.succeed(client.fetches.at(1).opId, flac('b'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        bridge.playableSource(QStringLiteral("$c"));
        client.succeed(client.fetches.at(2).opId, flac('c'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));

        QVERIFY(QFileInfo::exists(QUrl(urlA).toLocalFile()));
        QVERIFY(!bridge.playableSource(QStringLiteral("$a")).isEmpty());
        const int fetchesBefore = client.fetches.size();
        QVERIFY(bridge.playableSource(QStringLiteral("$b")).isEmpty());
        QCOMPARE(client.fetches.size(), fetchesBefore + 1);

        bridge.pinPlayable(QStringLiteral("$a"));
        bridge.unpinPlayable(QStringLiteral("$a"));
        bridge.playableSource(QStringLiteral("$x1"));
        client.succeed(client.fetches.last().opId, flac('x'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        bridge.playableSource(QStringLiteral("$x2"));
        client.succeed(client.fetches.last().opId, flac('y'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        QVERIFY(QFileInfo::exists(QUrl(urlA).toLocalFile()));

        bridge.unpinPlayable(QStringLiteral("$a"));
        bridge.playableSource(QStringLiteral("$d"));
        client.succeed(client.fetches.last().opId, flac('d'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        bridge.playableSource(QStringLiteral("$e"));
        client.succeed(client.fetches.last().opId, flac('e'),
                       QStringLiteral("audio/flac"));
        QVERIFY(waitForPlayableCount(ready, ++materialized));
        QVERIFY(!QFileInfo::exists(QUrl(urlA).toLocalFile()));
    }

    void thumbPayloadSniffingRejectsAvContainers()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);

        bridge.mediaSource(QStringLiteral("$vid"), QStringLiteral("thumb"));
        QByteArray mp4 = QByteArray("\x00\x00\x00\x18", 4)
            + QByteArray("ftypisom") + QByteArray(16, '\0');
        client.succeed(client.fetches.at(0).opId, mp4,
                       QStringLiteral("video/mp4"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));
        QCOMPARE(cached.count(), 0);
        QVERIFY(bridge.cachedBytes(QStringLiteral("thumb:$vid")).isEmpty());

        bridge.retry(QStringLiteral("thumb:$vid"));
        bridge.mediaSource(QStringLiteral("$vid"), QStringLiteral("thumb"));
        QByteArray jpeg;
        jpeg.append('\xff');
        jpeg.append('\xd8');
        jpeg.append('\xff');
        jpeg.append(QByteArray(16, 'j'));
        client.succeed(client.fetches.at(1).opId, jpeg,
                       QStringLiteral("video/mp4"));
        QCOMPARE(cached.count(), 1);
        QVERIFY(!bridge.cachedBytes(QStringLiteral("thumb:$vid")).isEmpty());

        bridge.mediaSource(QStringLiteral("$file"), QStringLiteral("full"));
        client.succeed(client.fetches.at(2).opId, mp4,
                       QStringLiteral("video/mp4"));
        QCOMPARE(cached.count(), 2);
    }

    void fullPayloadSniffingRejectsSvgTheSameWayAThumbnailDoes()
    {
        const QByteArray plainSvg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray xmlFirst =
            "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray svgz = QByteArray("\x1f\x8b", 2) + QByteArray(30, '\x08');

        int n = 0;
        for (const QByteArray &payload : { plainSvg, xmlFirst, svgz }) {
            FakeClient client;
            MediaBridge bridge;
            bridge.setClient(&client);
            QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
            QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
            const QString ev = QStringLiteral("$fullsvg%1").arg(n++);

            bridge.mediaSource(ev, QStringLiteral("full"));
            QCOMPARE(client.fetches.size(), 1);
            client.succeed(client.fetches.at(0).opId, payload,
                           QStringLiteral("image/png"));
            QCOMPARE(cached.count(), 0);
            QCOMPARE(failed.count(), 1);
            QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));
            QVERIFY(bridge.cachedBytes(QStringLiteral("full:") + ev).isEmpty());
        }
    }

    void aHostileAttachmentNameCannotEscapeTheChosenDirectory()
    {
        MediaBridge bridge;
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("../../.bashrc")),
                 QStringLiteral(".bashrc"));
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("..\\..\\evil.exe")),
                 QStringLiteral("evil.exe"));
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("a/b/c.png")),
                 QStringLiteral("c.png"));
        QVERIFY(bridge.suggestedSaveName(QStringLiteral("con.txt"))
                    .startsWith(QStringLiteral("file-")));
        QVERIFY(!bridge.suggestedSaveName(QStringLiteral("ok.png"))
                     .contains(QLatin1Char('/')));
        QCOMPARE(bridge.suggestedSaveName(QStringLiteral("ok.png")),
                 QStringLiteral("ok.png"));
        QVERIFY(bridge.suggestedSaveName(QString()).isEmpty());
        QVERIFY(bridge.suggestedSaveName(QStringLiteral("   ")).isEmpty());
        QVERIFY(bridge.suggestedSaveName(QStringLiteral("..")).isEmpty());
        const QString longName =
            bridge.suggestedSaveName(QString(400, QLatin1Char('x'))
                                     + QStringLiteral(".png"));
        QVERIFY(longName.size() <= 120);
        QVERIFY(longName.endsWith(QStringLiteral(".png")));
    }

    void bidiControlsAreStrippedFromSavedNames()
    {
        MediaBridge bridge;
        QCOMPARE(bridge.suggestedSaveName(
                     QStringLiteral("r\u202Etxt.exe\u202C.txt")),
                 QStringLiteral("rtxt.exe.txt"));
        QCOMPARE(bridge.suggestedSaveName(
                     QStringLiteral("a\u2066b\u2069\u200E\u200F\u061Cc.png")),
                 QStringLiteral("abc.png"));
        QCOMPARE(bridge.suggestedSaveName(
                     QStringLiteral("x\u200B\uFEFFy.png")),
                 QStringLiteral("xy.png"));
        QVERIFY(bridge.suggestedSaveName(
                    QStringLiteral("\U0001F468\u200D\U0001F469.png"))
                    .contains(QChar(0x200D)));
    }

    void aSaveIsListedWhileInFlightAndSurvivesARoomSwitch()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy listChanged(&bridge, &MediaBridge::activeSavesChanged);
        QSignalSpy saved(&bridge, &MediaBridge::saveFinished);

        QVERIFY(bridge.savingKeys().isEmpty());
        bridge.saveAs(QStringLiteral("$file"),
                      QUrl::fromLocalFile(dir.path() + QStringLiteral("/a.bin")));
        QCOMPARE(bridge.savingKeys(), QStringList{QStringLiteral("$file")});
        QCOMPARE(bridge.activeSaves().size(), 1);
        QCOMPARE(bridge.activeSaves().at(0).toMap().value(
                     QStringLiteral("fileName")).toString(),
                 QStringLiteral("a.bin"));

        bridge.dropQueuedSpeculative();
        QCOMPARE(bridge.savingKeys().size(), 1);
        QCOMPARE(client.cancels.size(), 0);

        client.succeed(client.fetches.at(0).opId, QByteArray("payload"),
                       QStringLiteral("text/plain"));
        QVERIFY(saved.wait(3000) || saved.count() > 0);
        QVERIFY(bridge.savingKeys().isEmpty());
        QVERIFY(listChanged.count() >= 2);
        QVERIFY(QFileInfo::exists(dir.path() + QStringLiteral("/a.bin")));
    }

    void cancellingASaveCancelsTheFetchAndWritesNothing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy saved(&bridge, &MediaBridge::saveFinished);

        bridge.saveAs(QStringLiteral("$file"),
                      QUrl::fromLocalFile(dir.path() + QStringLiteral("/b.bin")));
        QCOMPARE(client.fetches.size(), 1);
        const quint64 opId = client.fetches.at(0).opId;

        QSignalSpy cancelled(&bridge, &MediaBridge::saveCancelled);
        bridge.cancelSave(QStringLiteral("$file"));
        QCOMPARE(cancelled.count(), 1);
        QCOMPARE(cancelled.at(0).at(0).toString(), QStringLiteral("$file"));
        QVERIFY(bridge.savingKeys().isEmpty());
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(client.cancels.at(0), opId);
        QCOMPARE(saved.count(), 1);
        QCOMPARE(saved.at(0).at(0).toBool(), false);
        QCOMPARE(saved.at(0).at(2).toString(), QStringLiteral("$file"));

        client.succeed(opId, QByteArray("payload"), QStringLiteral("text/plain"));
        QTest::qWait(50);
        QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/b.bin")));
        QCOMPARE(saved.count(), 1);

        bridge.cancelSave(QStringLiteral("$other"));
        QCOMPARE(saved.count(), 1);
    }

    void saveIntoNeverReplacesAnExistingFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        {
            QFile existing(dir.filePath(QStringLiteral("a.bin")));
            QVERIFY(existing.open(QIODevice::WriteOnly));
            existing.write("old");
        }
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy completed(&bridge, &MediaBridge::saveCompleted);
        QSignalSpy saved(&bridge, &MediaBridge::saveFinished);

        bridge.saveInto(QStringLiteral("$file"), dir.path(),
                        QStringLiteral("a.bin"));
        QCOMPARE(client.fetches.size(), 1);
        client.succeed(client.fetches.at(0).opId, QByteArray("new"),
                       QStringLiteral("application/octet-stream"));
        QVERIFY(saved.count() > 0 || saved.wait(3000));
        QCOMPARE(saved.at(0).at(0).toBool(), true);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.at(0).at(0).toString(), QStringLiteral("$file"));
        const QString written = completed.at(0).at(1).toString();
        QCOMPARE(QFileInfo(written).fileName(), QStringLiteral("a (1).bin"));

        QFile oldFile(dir.filePath(QStringLiteral("a.bin")));
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        QCOMPARE(oldFile.readAll(), QByteArray("old"));
        QFile newFile(written);
        QVERIFY(newFile.open(QIODevice::ReadOnly));
        QCOMPARE(newFile.readAll(), QByteArray("new"));

        QSignalSpy completed2(&bridge, &MediaBridge::saveCompleted);
        bridge.saveAs(QStringLiteral("$file"),
                      QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.bin"))));
        QCOMPARE(client.fetches.size(), 2);
        client.succeed(client.fetches.at(1).opId, QByteArray("replaced"),
                       QStringLiteral("application/octet-stream"));
        QVERIFY(completed2.count() > 0 || completed2.wait(3000));
        QCOMPARE(QFileInfo(completed2.at(0).at(1).toString()).fileName(),
                 QStringLiteral("a.bin"));
    }

    void clearingTheSessionDropsTheSaveList()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.saveAs(QStringLiteral("$file"),
                      QUrl::fromLocalFile(dir.path() + QStringLiteral("/c.bin")));
        QCOMPARE(bridge.savingKeys().size(), 1);
        bridge.clear();
        QVERIFY(bridge.savingKeys().isEmpty());
    }

    void savingWithATraversingLeafStaysInTheReportedDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkpath(QStringLiteral("sub"));
        const QString outside = dir.path() + QStringLiteral("/pwned.txt");

        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy saved(&bridge, &MediaBridge::saveFinished);

        const QUrl destination = QUrl::fromLocalFile(
            dir.path() + QStringLiteral("/sub/../pwned.txt"));
        bridge.saveAs(QStringLiteral("$ev"), destination);
        QCOMPARE(client.fetches.size(), 1);
        client.succeed(client.fetches.at(0).opId, QByteArray("payload"),
                       QStringLiteral("text/plain"));
        QVERIFY(saved.wait(3000) || saved.count() > 0);

        QVERIFY2(!QFileInfo::exists(outside + QStringLiteral(".escaped")),
                 "the leaf escaped the resolved directory");
    }

    void thumbPayloadSniffingRejectsSvgAndItsCompressedSpelling()
    {
        const QByteArray plainSvg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray xmlFirst =
            "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        const QByteArray bomAndSpace =
            QByteArray("\xef\xbb\xbf", 3) + "\n\t  <svg/>";
        const QByteArray doctypeFirst = "<!DOCTYPE svg><svg/>";
        const QByteArray commentFirst = "<!-- hello --><svg/>";
        const QByteArray svgz = QByteArray("\x1f\x8b", 2) + QByteArray(30, '\x08');

        int n = 0;
        for (const QByteArray &payload : { plainSvg, xmlFirst, bomAndSpace,
                                           doctypeFirst, commentFirst, svgz }) {
            FakeClient client;
            MediaBridge bridge;
            bridge.setClient(&client);
            QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
            QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
            const QString ev = QStringLiteral("$svg%1").arg(n++);

            bridge.mediaSource(ev, QStringLiteral("thumb"));
            client.succeed(client.fetches.at(0).opId, payload,
                           QStringLiteral("image/png"));
            QCOMPARE(cached.count(), 0);
            QCOMPARE(failed.count(), 1);
            QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));
            QVERIFY(bridge.cachedBytes(QStringLiteral("thumb:") + ev).isEmpty());
        }

        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$png"), QStringLiteral("thumb"));
        QByteArray png = QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(16, 'p');
        client.succeed(client.fetches.at(0).opId, png,
                       QStringLiteral("image/png"));
        QCOMPARE(cached.count(), 1);
    }

    void anSvgRowShowsOnlyARasterThumbnailAndNeverTheSvg()
    {
        const QByteArray svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect/></svg>";
        const QByteArray png =
            QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(16, 'p');

        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);

        bridge.mediaSource(QStringLiteral("$svgrow"), QStringLiteral("thumb"));
        client.succeed(client.fetches.at(0).opId, png,
                       QStringLiteral("image/svg+xml"));
        QCOMPARE(bridge.cachedBytes(QStringLiteral("thumb:$svgrow")), png);
        QVERIFY(bridge.cachedSource(QStringLiteral("thumb:$svgrow"))
                    .startsWith(QStringLiteral("image://lightning-media/")));

        bridge.mediaSource(QStringLiteral("$svgrow"), QStringLiteral("full"));
        client.succeed(client.fetches.at(1).opId, svg,
                       QStringLiteral("image/svg+xml"));
        QVERIFY(bridge.cachedBytes(QStringLiteral("full:$svgrow")).isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("full:$svgrow"));
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("rejected"));

        bridge.mediaSource(QStringLiteral("$disguised"),
                           QStringLiteral("thumb"));
        client.succeed(client.fetches.at(2).opId, svg,
                       QStringLiteral("image/png"));
        QVERIFY(bridge.cachedBytes(QStringLiteral("thumb:$disguised")).isEmpty());
        QCOMPARE(failed.count(), 2);
        QCOMPARE(failed.at(1).at(1).toString(), QStringLiteral("rejected"));
        bridge.mediaSource(QStringLiteral("$disguised"),
                           QStringLiteral("thumb"));
        QCOMPARE(client.fetches.size(), 3);
    }

    void droppedSpeculativeQueueEntriesNeverDispatch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        bridge.animatedSource(QStringLiteral("$gif"));
        bridge.mediaSource(QStringLiteral("$keep"), QStringLiteral("thumb"));
        QCOMPARE(bridge.queuedCountForTest(), 2);
        bridge.dropQueuedSpeculative();
        QCOMPARE(bridge.queuedCountForTest(), 1);
        client.succeed(client.fetches.at(0).opId, QByteArray("img"));
        QCOMPARE(client.fetches.last().key, QStringLiteral("$keep"));
        client.succeed(client.fetches.at(1).opId, QByteArray("img"));
        QCOMPARE(client.fetches.size(), 9);
    }

    void cancelPlayableAbortsBackendAndFreesSlot()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.playableSource(QStringLiteral("$video"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        const quint64 opId = client.fetches.first().opId;
        bridge.cancelPlayable(QStringLiteral("$video"));
        QCOMPARE(bridge.inflightCountForTest(), 0);
        QCOMPARE(client.cancels, QList<quint64>{opId});
        QVERIFY(bridge.failureCategory(QStringLiteral("full:$video")).isEmpty());
        bridge.playableSource(QStringLiteral("$video"));
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.last().timeoutClass, 1);
    }

    void lateCompletionAfterCancelIsDropped()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.playableSource(QStringLiteral("$video"));
        const quint64 opId = client.fetches.first().opId;
        bridge.cancelPlayable(QStringLiteral("$video"));
        client.succeed(opId, QByteArray("GIF89a-not-really"),
                       QStringLiteral("video/mp4"));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QTest::qWait(50);
        QCOMPARE(ready.count(), 0);
        QVERIFY(bridge.cachedSource(QStringLiteral("full:$video")).isEmpty());
    }

    void cancelKeepsFetchAliveForOtherConsumers()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.animatedSource(QStringLiteral("$shared"));
        bridge.playableSource(QStringLiteral("$shared"));
        QCOMPARE(client.fetches.size(), 1);
        bridge.cancelPlayable(QStringLiteral("$shared"));
        QCOMPARE(client.cancels.size(), 0);
        QCOMPARE(bridge.inflightCountForTest(), 1);
    }

    void prefetchPlayableHonorsCallerCap()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const double mb = 1024.0 * 1024.0;
        bridge.prefetchPlayable(QStringLiteral("$48"), 48 * mb);
        QCOMPARE(client.fetches.size(), 0);
        bridge.prefetchPlayable(QStringLiteral("$48"), 48 * mb, 64 * mb);
        QCOMPARE(client.fetches.size(), 1);
        bridge.prefetchPlayable(QStringLiteral("$8"), 8 * mb, 4 * mb);
        QCOMPARE(client.fetches.size(), 1);
        bridge.prefetchPlayable(QStringLiteral("$8"), 8 * mb, 16 * mb);
        QCOMPARE(client.fetches.size(), 2);
    }

    void prefetchPlayableHonorsSizeCap()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.prefetchPlayable(QStringLiteral("$big"),
                                512.0 * 1024 * 1024);
        bridge.prefetchPlayable(QStringLiteral("$unknown"), 0);
        QCOMPARE(client.fetches.size(), 0);
        bridge.prefetchPlayable(QStringLiteral("$small"), 4 * 1024 * 1024);
        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().timeoutClass, 1);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        QByteArray mp4(1024, 'x');
        mp4.replace(4, 4, "ftyp");
        client.succeed(client.fetches.first().opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 1));
        QVERIFY(!bridge.playableSource(QStringLiteral("$small")).isEmpty());
        QCOMPARE(client.fetches.size(), 1);
    }

    void queuedPrefetchDroppedOnRoomSwitch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 8; ++i)
            bridge.mediaSource(QStringLiteral("$m%1").arg(i),
                               QStringLiteral("thumb"));
        bridge.prefetchPlayable(QStringLiteral("$spec"), 1024 * 1024);
        QCOMPARE(bridge.queuedCountForTest(), 1);
        bridge.dropQueuedSpeculative();
        QCOMPARE(bridge.queuedCountForTest(), 0);
    }

    void cancelWithTwoPlayableConsumersKeepsFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.playableSource(QStringLiteral("$dual"));
        bridge.playableSource(QStringLiteral("$dual"));
        QCOMPARE(client.fetches.size(), 1);
        bridge.cancelPlayable(QStringLiteral("$dual"));
        QCOMPARE(client.cancels.size(), 0);
        QCOMPARE(bridge.inflightCountForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$dual"));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    void posterHookForOverCapVideoDoesNotBlockCancel()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.videoPosterSource(QStringLiteral("$huge"),
                                 500.0 * 1024 * 1024);
        QCOMPARE(client.fetches.size(), 0);
        bridge.playableSource(QStringLiteral("$huge"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        const quint64 opId = client.fetches.first().opId;
        bridge.cancelPlayable(QStringLiteral("$huge"));
        QCOMPARE(client.cancels, QList<quint64>{opId});
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    void aCachedAnimationLeavesNoInterestBehindToVetoACancel()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QByteArray gif("GIF89a");
        gif.append(QByteArray(64, '\0'));
        bridge.mediaSource(QStringLiteral("$gif"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, gif,
                       QStringLiteral("image/gif"));
        QCOMPARE(client.fetches.size(), 1);

        QVERIFY(bridge.animatedSource(QStringLiteral("$gif"))
                    .startsWith(QStringLiteral("file://")));
        QCOMPARE(client.fetches.size(), 1);

        bridge.playableSource(QStringLiteral("$gif"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        const quint64 opId = client.fetches.last().opId;
        bridge.cancelPlayable(QStringLiteral("$gif"));
        QCOMPARE(client.cancels, QList<quint64>{opId});
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    void aCachedPayloadThatIsNotAnAnimationAnswersTheDemandingCaller()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy failed(&bridge, &MediaBridge::mediaFetchFailed);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QByteArray png("\x89PNG\r\n\x1a\n", 8);
        png.append(QByteArray(64, '\0'));
        bridge.mediaSource(QStringLiteral("$png"), QStringLiteral("full"));
        client.succeed(client.fetches.first().opId, png, QString());
        QCOMPARE(failed.count(), 0);

        QCOMPARE(bridge.animatedSource(QStringLiteral("$png")), QString());
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(),
                 QStringLiteral("full:$png"));
        QCOMPARE(failed.first().at(1).toString(),
                 QStringLiteral("invalid_gif"));
        QVERIFY(bridge.failureCategory(QStringLiteral("full:$png")).isEmpty());

        bridge.mediaSource(QStringLiteral("$sticker"), QStringLiteral("full"));
        client.succeed(client.fetches.last().opId, png, QString());
        QCOMPARE(bridge.animatedSource(QStringLiteral("$sticker"),
true),
                 QString());
        QCOMPARE(failed.count(), 1);
    }

    void terminalFailureClearsInterestSets()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.prefetchPlayable(QStringLiteral("$flaky"), 1024 * 1024);
        QCOMPARE(client.fetches.size(), 1);
        client.fail(client.fetches.first().opId, QStringLiteral("network"));
        bridge.retry(QStringLiteral("full:$flaky"));
        bridge.playableSource(QStringLiteral("$flaky"));
        QCOMPARE(client.fetches.size(), 2);
        bridge.cancelPlayable(QStringLiteral("$flaky"));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    void ramCacheHitRetiresItsInterestWhenTheWriteLands()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::playableMediaReady);
        bridge.setPlayableCapsForTest(1, 1024 * 1024);
        QByteArray mp4(256, 'x');
        mp4.replace(4, 4, "ftyp");
        bridge.playableSource(QStringLiteral("$track"));
        client.succeed(client.fetches.at(0).opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 1));
        bridge.playableSource(QStringLiteral("$other"));
        client.succeed(client.fetches.at(1).opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 2));
        QVERIFY(bridge.playableSource(QStringLiteral("$track")).isEmpty());
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 1);
        QCOMPARE(client.fetches.size(), 2);
        QVERIFY(waitForPlayableCount(ready, 3));
        QCOMPARE(bridge.pendingPlayableWritesForTest(), 0);
        QVERIFY(!bridge.playableSource(QStringLiteral("$track")).isEmpty());
        bridge.setCacheLimitBytes(1);
        bridge.mediaSource(QStringLiteral("$bump"), QStringLiteral("full"));
        client.succeed(client.fetches.at(2).opId, QByteArray("img"));
        bridge.playableSource(QStringLiteral("$other"));
        client.succeed(client.fetches.at(3).opId, mp4,
                       QStringLiteral("video/mp4"));
        QVERIFY(waitForPlayableCount(ready, 4));
        bridge.playableSource(QStringLiteral("$track"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$track"));
        QCOMPARE(client.cancels.size(), 1);
        QCOMPARE(bridge.inflightCountForTest(), 0);
    }

    void cancelWithoutPlayableInterestIsNoop()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.mediaSource(QStringLiteral("$img"), QStringLiteral("full"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        bridge.cancelPlayable(QStringLiteral("$img"));
        QCOMPARE(bridge.inflightCountForTest(), 1);
        QCOMPARE(client.cancels.size(), 0);
    }

    void aWidthOnlySourceSizeBoundsTheDecode()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        MediaImageProvider provider(&bridge);

        QImage big(2400, 1600, QImage::Format_RGB32);
        big.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(big.save(&buffer, "PNG"));
        buffer.close();

        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$ev"), QStringLiteral("full"));
        QVERIFY(!client.fetches.isEmpty());
        client.succeed(client.fetches.first().opId, png);
        QCOMPARE(cached.count(), 1);
        const QString id = cached.first().at(0).toString();

        QSize reported;
        const QImage bounded = provider.requestImage(id, &reported,
                                                     QSize(640, 0));
        QVERIFY2(!bounded.isNull(), "the bounded decode produced nothing");
        QCOMPARE(bounded.width(), 640);
        QCOMPARE(bounded.height(), 427);
    }

    void noSourceSizeDecodesNaturally()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        MediaImageProvider provider(&bridge);

        QImage big(1200, 900, QImage::Format_RGB32);
        big.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(big.save(&buffer, "PNG"));
        buffer.close();

        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$ev2"), QStringLiteral("full"));
        QVERIFY(!client.fetches.isEmpty());
        client.succeed(client.fetches.first().opId, png);
        QCOMPARE(cached.count(), 1);
        const QString id = cached.first().at(0).toString();

        QSize reported;
        QCOMPARE(provider.requestImage(id, &reported, QSize()).size(),
                 QSize(1200, 900));
        QCOMPARE(provider.requestImage(id, &reported, QSize(4000, 0)).size(),
                 QSize(1200, 900));
        QCOMPARE(provider.requestImage(id, &reported, QSize(0, 300)).size(),
                 QSize(400, 300));
    }

    void aBakedShapeStillHonoursAnUpscale()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        MediaImageProvider provider(&bridge);

        QImage small(100, 100, QImage::Format_RGB32);
        small.fill(Qt::green);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(small.save(&buffer, "PNG"));
        buffer.close();

        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.mediaSource(QStringLiteral("$av"), QStringLiteral("full"));
        QVERIFY(!client.fetches.isEmpty());
        client.succeed(client.fetches.first().opId, png);
        QCOMPARE(cached.count(), 1);
        const QString id = cached.first().at(0).toString();

        QSize reported;
        const QImage masked = provider.requestImage(
            id + QStringLiteral("|shape:circle"), &reported, QSize(224, 224));
        QCOMPARE(masked.size(), QSize(224, 224));
        QCOMPARE(provider.requestImage(id, &reported, QSize(224, 224)).size(),
                 QSize(100, 100));
    }

    void theAnimationSniffCountsFramesAndReadsTheCanvas()
    {
        namespace sniff = lightning::animsniff;
        QCOMPARE(sniff::gifFrameCount(makeGif(40, 30, 1)), 1);
        QCOMPARE(sniff::gifFrameCount(makeGif(40, 30, 3), 9), 3);
        QVERIFY(!sniff::isAnimation(makeGif(40, 30, 1)));
        QVERIFY(sniff::isAnimation(makeGif(40, 30, 2)));
        const QByteArray two = makeGif(40, 30, 2);
        QCOMPARE(sniff::gifFrameCount(two.left(two.size() - 4)), 1);
        QCOMPARE(sniff::canvasSize(two), QSize(40, 30));
        QCOMPARE(sniff::animationSuffix(two), QStringLiteral("gif"));
        QVERIFY(sniff::isAnimation(makeWebpHeader(640, 200, true)));
        QVERIFY(!sniff::isAnimation(makeWebpHeader(640, 200, false)));
        QCOMPARE(sniff::canvasSize(makeWebpHeader(640, 200, true)),
                 QSize(640, 200));
        QCOMPARE(sniff::animationSuffix(makeWebpHeader(640, 200, true)),
                 QStringLiteral("webp"));
        QVERIFY(!sniff::isAnimation(QByteArrayLiteral("<svg/>")));
        QVERIFY(!sniff::isAnimation(pngBytes()));
        QByteArray bytes = makeGif(40, 30, 3);
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "gif");
        QCOMPARE(reader.imageCount(), 3);
    }

    void aPassiveProbeSkipsJpegThumbnailsAndAnExplicitOneDoesNot()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QCOMPARE(bridge.avatarAnimationSource(kMxc, false), QString());
        QCOMPARE(client.fetches.size(), 0);

        bridge.avatarSource(kMxc, 40);
        QCOMPARE(client.fetches.size(), 1);
        client.succeed(client.fetches.first().opId, jpegBytes(),
                       QStringLiteral("image/jpeg"));
        QCOMPARE(bridge.avatarAnimationSource(kMxc, false), QString());
        QCOMPARE(client.fetches.size(), 1);

        QCOMPARE(bridge.avatarAnimationSource(kMxc, true), QString());
        QCOMPARE(client.fetches.size(), 2);
        QCOMPARE(client.fetches.last().key, kMxc);
        QCOMPARE(client.fetches.last().width, 0);
        QCOMPARE(client.fetches.last().height, MediaBridge::kMotionProbeHeight);
    }

    void aProbedAnimationBecomesAFileAndASignal()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        bridge.avatarSource(kMxc, 40);
        client.succeed(client.fetches.first().opId, pngBytes());
        QCOMPARE(bridge.avatarAnimationSource(kMxc, false), QString());
        QCOMPARE(client.fetches.size(), 2);
        bridge.avatarAnimationSource(kMxc, false);
        QCOMPARE(client.fetches.size(), 2);

        client.succeed(client.fetches.last().opId, makeGif(64, 64, 2),
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(),
                 QStringLiteral("motion:") + kMxc);
        const QString url = bridge.avatarAnimationSource(kMxc, false);
        QVERIFY(url.startsWith(QStringLiteral("file://")));
        const QString path = QUrl(url).toLocalFile();
        QVERIFY(path.endsWith(QStringLiteral(".gif")));
        QCOMPARE(QFileInfo(path).permissions()
                     & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                        | QFileDevice::ReadOther | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        client.logout();
        QVERIFY(!QFileInfo::exists(path));
    }

    void aStillOriginalIsRememberedAndNeverFetchedAgain()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        QSignalSpy cached(&bridge, &MediaBridge::mediaCached);
        bridge.avatarAnimationSource(kMxc, true);
        QCOMPARE(client.fetches.size(), 1);
        const qint64 before = bridge.cacheBytesUsed();
        client.succeed(client.fetches.first().opId, makeGif(64, 64, 1),
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(cached.count(), 0);
        QCOMPARE(bridge.cacheBytesUsed(), before);
        QCOMPARE(bridge.avatarAnimationSource(kMxc, true), QString());
        QCOMPARE(bridge.avatarAnimationSource(kMxc, false), QString());
        QCOMPARE(client.fetches.size(), 1);
        client.logout();
        bridge.avatarAnimationSource(kMxc, true);
        QCOMPARE(client.fetches.size(), 2);
    }

    void anAnimatedThumbnailNeedsNoSecondFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.avatarSource(kMxc, 40);
        client.succeed(client.fetches.first().opId, makeGif(64, 64, 2),
                       QStringLiteral("image/gif"));
        QVERIFY(bridge.avatarAnimationSource(kMxc, false)
                    .startsWith(QStringLiteral("file://")));
        QCOMPARE(client.fetches.size(), 1);
    }

    void aStagedAnimationPlaysFromTheStoreAndNothingElseDoes()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString prefix = QStringLiteral("image://lightning-staged/");
        QHash<QString, QByteArray> store;
        const auto add = [&store](const QByteArray &bytes) {
            const QString token = QString::number(store.size() + 1);
            store.insert(token, bytes);
            return token;
        };
        const QString gif = add(makeGif(64, 64, 2));
        const QString still = add(makeGif(64, 64, 1));
        QVERIFY(!gif.isEmpty());
        QCOMPARE(bridge.stagedAnimationSource(prefix + gif), QString());
        bridge.setStagedBytesProvider([&store](const QString &token) {
            return store.value(token);
        });
        const QString url = bridge.stagedAnimationSource(prefix + gif);
        QVERIFY(url.startsWith(QStringLiteral("file://")));
        QVERIFY(QFileInfo::exists(QUrl(url).toLocalFile()));
        QCOMPARE(bridge.stagedAnimationSource(prefix + gif), url);
        QCOMPARE(bridge.stagedAnimationSource(prefix + still), QString());
        QCOMPARE(bridge.stagedAnimationSource(prefix + QStringLiteral("nope")),
                 QString());
        QCOMPARE(bridge.stagedAnimationSource(QStringLiteral("file:///tmp/a.gif")),
                 QString());
        QCOMPARE(bridge.stagedAnimationSource(QStringLiteral("mxc://example.org/a")),
                 QString());
        QCOMPARE(client.fetches.size(), 0);
        const QString huge = add(makeGif(4096, 2048, 2));
        QCOMPARE(bridge.stagedAnimationSource(prefix + huge), QString());
    }

    void aBackdropAnimationRefusesFlashingAndEndlessFiles()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const auto cacheAs = [&](const QString &mxc, const QByteArray &bytes) {
            bridge.wideImageSource(mxc);
            client.succeed(client.fetches.last().opId, bytes,
                           QStringLiteral("image/gif"));
        };
        const auto withDelay = [](QByteArray gif, char cs) {
            gif.replace(QByteArray("\x21\xf9\x04\x00\x0a\x00", 6),
                        QByteArray("\x21\xf9\x04\x00", 4) + QByteArray(1, cs)
                            + QByteArray(1, '\0'));
            return gif;
        };
        const QString flashing = QStringLiteral("mxc://example.org/flash");
        cacheAs(flashing, withDelay(makeGif(64, 64, 4), 3));
        QCOMPARE(bridge.backdropAnimationSource(flashing), QString());
        QVERIFY(bridge.wideAnimationSource(flashing).startsWith(QStringLiteral("file://")));

        const QString endless = QStringLiteral("mxc://example.org/endless");
        cacheAs(endless, makeGif(64, 64, 320));
        QCOMPARE(bridge.backdropAnimationSource(endless), QString());

        const QString slow = QStringLiteral("mxc://example.org/slow");
        cacheAs(slow, withDelay(makeGif(64, 64, 4), 1));
        QVERIFY(bridge.backdropAnimationSource(slow).startsWith(QStringLiteral("file://")));
        const QString calm = QStringLiteral("mxc://example.org/calm");
        cacheAs(calm, makeGif(64, 64, 4));
        QVERIFY(bridge.backdropAnimationSource(calm).startsWith(QStringLiteral("file://")));
    }

    void theCanvasBoundIsPerRoleAndMarkupNeverPlays()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QByteArray big = makeGif(2048, 1024, 2);
        bridge.avatarAnimationSource(kMxc, true);
        client.succeed(client.fetches.last().opId, big,
                       QStringLiteral("image/gif"));
        QCOMPARE(bridge.avatarAnimationSource(kMxc, true), QString());
        const int fetched = client.fetches.size();
        QCOMPARE(bridge.avatarAnimationSource(kMxc, true), QString());
        QCOMPARE(client.fetches.size(), fetched);

        QCOMPARE(bridge.wideAnimationSource(kMxc), QString());
        QCOMPARE(client.fetches.size(), fetched);
        bridge.wideImageSource(kMxc);
        client.succeed(client.fetches.last().opId, big,
                       QStringLiteral("image/gif"));
        QVERIFY(bridge.wideAnimationSource(kMxc)
                    .startsWith(QStringLiteral("file://")));
        QCOMPARE(bridge.avatarAnimationSource(kMxc, true), QString());

        const QString svg = QStringLiteral("mxc://example.org/svg");
        bridge.avatarAnimationSource(svg, true);
        client.succeed(client.fetches.last().opId,
                       QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"),
                       QStringLiteral("image/gif"));
        QCOMPARE(bridge.failureCategory(QStringLiteral("motion:") + svg),
                 QStringLiteral("rejected"));
        QCOMPARE(bridge.avatarAnimationSource(svg, true), QString());
        bridge.wideImageSource(svg);
        QCOMPARE(bridge.wideAnimationSource(svg), QString());
    }

    void aStillBannerHasNoAnimation()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.wideImageSource(kMxc);
        client.succeed(client.fetches.last().opId, pngBytes());
        QCOMPARE(bridge.wideAnimationSource(kMxc), QString());
        QCOMPARE(bridge.wideAnimationSource(kMxc), QString());
        QCOMPARE(client.fetches.size(), 1);
    }

    void motionSlotsAreBoundedAndFreedWithTheirOwner()
    {
        MediaBridge bridge;
        QSignalSpy freed(&bridge, &MediaBridge::motionSlotFreed);
        QList<QObject *> owners;
        for (int i = 0; i < MediaBridge::kMaxMotionSlots + 1; ++i)
            owners.append(new QObject(&bridge));
        for (int i = 0; i < MediaBridge::kMaxMotionSlots; ++i)
            QVERIFY(bridge.claimMotionSlot(owners.at(i)));
        QVERIFY(bridge.claimMotionSlot(owners.first()));
        QCOMPARE(bridge.motionSlotsInUseForTest(), MediaBridge::kMaxMotionSlots);
        QVERIFY(!bridge.claimMotionSlot(owners.last()));
        QVERIFY(!bridge.claimMotionSlot(nullptr));
        QList<QObject *> intents;
        for (int i = 0; i < MediaBridge::kMotionIntentReserve + 1; ++i)
            intents.append(new QObject(&bridge));
        for (int i = 0; i < MediaBridge::kMotionIntentReserve; ++i)
            QVERIFY(bridge.claimMotionSlot(intents.at(i), true));
        QVERIFY(!bridge.claimMotionSlot(intents.last(), true));
        for (int i = 0; i < MediaBridge::kMotionIntentReserve; ++i)
            bridge.releaseMotionSlot(intents.at(i));
        QCOMPARE(freed.count(), 0);
        QTRY_COMPARE(freed.count(), 1);
        QCoreApplication::processEvents();
        QCOMPARE(freed.count(), 1);
        freed.clear();

        bridge.releaseMotionSlot(owners.at(1));
        QTRY_COMPARE(freed.count(), 1);
        bridge.releaseMotionSlot(owners.at(1));
        QCoreApplication::processEvents();
        QCOMPARE(freed.count(), 1);
        QVERIFY(bridge.claimMotionSlot(owners.last()));

        delete owners.at(2);
        QCOMPARE(bridge.motionSlotsInUseForTest(),
                 MediaBridge::kMaxMotionSlots - 1);
        QTRY_COMPARE(freed.count(), 2);
    }

    void anIntentSlotIsGivenBackWhenTheIntentEnds()
    {
        MediaBridge bridge;
        QObject hoverA, hoverB;
        QList<QObject *> passive;
        for (int i = 0; i < MediaBridge::kMaxMotionSlots; ++i)
            passive.append(new QObject(&bridge));
        for (int i = 0; i < MediaBridge::kMaxMotionSlots - 1; ++i)
            QVERIFY(bridge.claimMotionSlot(passive.at(i)));
        QVERIFY(bridge.claimMotionSlot(&hoverA, true));
        QVERIFY(bridge.claimMotionSlot(&hoverA, false));
        QVERIFY(!bridge.claimMotionSlot(passive.last()));
        QVERIFY(bridge.claimMotionSlot(&hoverB, true));
        QCOMPARE(bridge.motionSlotsInUseForTest(),
                 MediaBridge::kMaxMotionSlots + 1);
        QVERIFY(!bridge.claimMotionSlot(&hoverB, false));
        QCOMPARE(bridge.motionSlotsInUseForTest(), MediaBridge::kMaxMotionSlots);
        for (int i = 0; i < 2 * MediaBridge::kMotionIntentReserve; ++i) {
            QObject hover;
            QVERIFY(bridge.claimMotionSlot(&hover, true));
            QVERIFY(!bridge.claimMotionSlot(&hover, false));
        }
        bridge.releaseMotionSlot(&hoverA);
    }

    void anOversizeOrEmptyCanvasAnimationNeverPlays()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QSignalSpy ready(&bridge, &MediaBridge::animatedMediaReady);
        const QByteArray huge =
            makeGif(64, 64, 2, gifComment(QByteArray(8 * 1024 * 1024, 'x')));
        QVERIFY(lightning::animsniff::isAnimation(huge));
        bridge.avatarAnimationSource(kMxc, true);
        client.succeed(client.fetches.last().opId, huge,
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        const int fetched = client.fetches.size();
        QCOMPARE(bridge.avatarAnimationSource(kMxc, true), QString());
        QCOMPARE(client.fetches.size(), fetched);

        const QString flat = QStringLiteral("mxc://example.org/flat");
        const QByteArray zeroWidth = makeGif(0, 64, 2);
        QVERIFY(lightning::animsniff::isAnimation(zeroWidth));
        bridge.avatarAnimationSource(flat, true);
        client.succeed(client.fetches.last().opId, zeroWidth,
                       QStringLiteral("image/gif"));
        QCOMPARE(ready.count(), 0);
        QCOMPARE(bridge.avatarAnimationSource(flat, true), QString());
        bridge.wideImageSource(flat);
        client.succeed(client.fetches.last().opId, zeroWidth,
                       QStringLiteral("image/gif"));
        QCOMPARE(bridge.wideAnimationSource(flat), QString());
    }

    void anExplicitProbeUsesTheHeavyLane()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        for (int i = 0; i < 6; ++i)
            bridge.mediaSource(QStringLiteral("$full%1").arg(i),
                               QStringLiteral("full"));
        QCOMPARE(bridge.inflightCountForTest(), 6);
        bridge.avatarAnimationSource(kMxc, true);
        QCOMPARE(bridge.inflightCountForTest(), 6);
        QCOMPARE(bridge.queuedCountForTest(), 1);
        bridge.avatarSource(QStringLiteral("mxc://example.org/other"), 40);
        QCOMPARE(bridge.inflightCountForTest(), 7);
    }

    void aTooLargeProbeIsNotRetried()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        bridge.setFailureRetryMsForTest(0);
        bridge.avatarAnimationSource(kMxc, true);
        QCOMPARE(client.fetches.size(), 1);
        client.fail(client.fetches.last().opId, QStringLiteral("too_large"));
        bridge.checkInflightTimeouts();
        QCOMPARE(bridge.avatarAnimationSource(kMxc, true), QString());
        QCOMPARE(client.fetches.size(), 1);
        const QString other = QStringLiteral("mxc://example.org/net");
        bridge.avatarAnimationSource(other, true);
        client.fail(client.fetches.last().opId, QStringLiteral("network"));
        bridge.checkInflightTimeouts();
        bridge.avatarAnimationSource(other, true);
        QCOMPARE(client.fetches.size(), 3);
    }

    void gifMetadataIsStrippedAndTheAnimationKept()
    {
        namespace sniff = lightning::animsniff;
        const QByteArray secret("GPS 54.6872N 25.2797E, taken by my phone");
        const QByteArray xmp =
            QByteArrayLiteral("<x:xmpmeta>") + secret + "</x:xmpmeta>";
        const QByteArray dirty = makeGif(
            64, 64, 3,
            gifComment(secret) + gifAppExtension("XMP DataXMP", xmp)
                + gifAppExtension("ICCRGBG1012", QByteArray(40, 'i')));
        QVERIFY(dirty.contains(secret));
        const QByteArray clean = sniff::stripGifMetadata(dirty);
        QVERIFY(!clean.isEmpty());
        QVERIFY(!clean.contains(secret));
        QVERIFY(!clean.contains("XMP DataXMP"));
        QVERIFY(!clean.contains("ICCRGBG1"));
        QVERIFY(clean.contains("NETSCAPE2.0"));
        QCOMPARE(sniff::gifFrameCount(clean, 9), 3);
        QCOMPARE(sniff::canvasSize(clean), QSize(64, 64));
        const QByteArray plain = makeGif(64, 64, 3);
        QCOMPARE(sniff::stripGifMetadata(plain), plain);
        QByteArray bytes = clean;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "gif");
        QCOMPARE(reader.imageCount(), 3);
        QCOMPARE(sniff::stripGifMetadata(plain + secret), plain);
    }

    void webpMetadataIsStrippedAndTheAnimationKept()
    {
        namespace sniff = lightning::animsniff;
        const QByteArray secret("GPS 54.6872N 25.2797E");
        const QByteArray webp = realAnimatedWebp();
        QVERIFY(sniff::isAnimation(webp));
        const QByteArray dirty = withWebpMetadata(
            webp, QByteArray("Exif\0\0", 6) + secret,
            QByteArrayLiteral("<x:xmpmeta>") + secret + "</x:xmpmeta>");
        QVERIFY(dirty.contains(secret));
        const QByteArray clean = sniff::stripWebpMetadata(dirty);
        QVERIFY(!clean.isEmpty());
        QVERIFY(!clean.contains(secret));
        QVERIFY(!clean.contains("EXIF"));
        QVERIFY(!clean.contains("XMP "));
        QCOMPARE(clean, webp);
        QVERIFY(sniff::isAnimation(clean));
        QCOMPARE(sniff::stripWebpMetadata(webp + secret), webp);
        for (const char *fourcc : { "Exif", "zTXt" }) {
            QByteArray vendor = webp + riffChunk(fourcc, secret);
            const quint32 riff = quint32(vendor.size() - 8);
            for (int i = 0; i < 4; ++i)
                vendor[4 + i] = char((riff >> (8 * i)) & 0xff);
            QVERIFY(vendor.contains(secret));
            QCOMPARE(sniff::stripWebpMetadata(vendor), webp);
        }
        if (QImageReader::supportedImageFormats().contains("webp")) {
            QByteArray bytes = clean;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "webp");
            QCOMPARE(reader.imageCount(), 2);
        }
    }

    void malformedAnimationsAreRefusedByTheStripper()
    {
        namespace sniff = lightning::animsniff;
        const QByteArray gif = makeGif(64, 64, 2, gifComment("note"));
        QVERIFY(sniff::stripGifMetadata(gif.left(gif.size() - 1)).isEmpty());
        QVERIFY(sniff::stripGifMetadata(gif.left(40)).isEmpty());
        QByteArray badApp = makeGif(64, 64, 2,
                                    QByteArray("\x21\xff\x05short\x00", 9));
        QVERIFY(sniff::stripGifMetadata(badApp).isEmpty());
        QByteArray badTag = makeGif(64, 64, 2, QByteArray("\x42", 1));
        QVERIFY(sniff::stripGifMetadata(badTag).isEmpty());

        const QByteArray webp = realAnimatedWebp();
        QByteArray longRiff = webp;
        longRiff[4] = char(0xff);
        QVERIFY(sniff::stripWebpMetadata(longRiff).isEmpty());
        QByteArray longChunk = webp;
        longChunk[17] = char(0xff);
        QVERIFY(sniff::stripWebpMetadata(longChunk).isEmpty());
        QVERIFY(sniff::stripWebpMetadata(webp.left(30)).isEmpty());
        QVERIFY(sniff::stripAnimationMetadata(pngBytes()).isEmpty());
        QVERIFY(sniff::stripAnimationMetadata(
                    QByteArrayLiteral("<svg xmlns=\"x\"/>")).isEmpty());
    }

    void aHeldAnimationIsNeverEvicted()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString heldPath =
            writeAnimation(client, bridge, QStringLiteral("mxc://x/held"));
        QVERIFY(QFile::exists(heldPath));
        auto *owner = new QObject;
        QVERIFY(bridge.holdAnimation(owner, QUrl::fromLocalFile(heldPath)
                                                .toString()));
        QCOMPARE(bridge.animationHoldsForTest(), 1);
        QString firstOther;
        for (int i = 0; i < 70; ++i) {
            const QString path = writeAnimation(
                client, bridge, QStringLiteral("mxc://x/other%1").arg(i));
            QVERIFY(!path.isEmpty());
            if (i == 0)
                firstOther = path;
        }
        QVERIFY(!QFile::exists(firstOther));
        QVERIFY2(QFile::exists(heldPath), "a held animation was evicted");

        delete owner;
        QCOMPARE(bridge.animationHoldsForTest(), 0);
        writeAnimation(client, bridge, QStringLiteral("mxc://x/last"));
        QVERIFY(!QFile::exists(heldPath));
    }

    void heldFilesStillMeetTheHardCeiling()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        QObject owners;
        QStringList paths;
        const int total = MediaBridge::kAnimatedHardCapEntries + 2;
        for (int i = 0; i < total; ++i) {
            const QString path = writeAnimation(
                client, bridge, QStringLiteral("mxc://x/row%1").arg(i));
            QVERIFY(!path.isEmpty());
            QVERIFY(bridge.holdAnimation(new QObject(&owners),
                                         QUrl::fromLocalFile(path).toString()));
            paths.append(path);
        }
        int existing = 0;
        for (const QString &path : std::as_const(paths))
            existing += QFile::exists(path) ? 1 : 0;
        QVERIFY(existing > 64);
        QCOMPARE(existing, MediaBridge::kAnimatedHardCapEntries);
        QVERIFY(!QFile::exists(paths.first()));
        QVERIFY(QFile::exists(paths.last()));
    }

    void onlyTheBridgesOwnFilesCanBeHeld()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString path =
            writeAnimation(client, bridge, QStringLiteral("mxc://x/own"));
        QObject owner;
        QVERIFY(bridge.holdAnimation(&owner,
                                     QUrl::fromLocalFile(path).toString()));
        QVERIFY(!bridge.holdAnimation(&owner, QStringLiteral(
                                                  "file:///etc/passwd")));
        QCOMPARE(bridge.animationHoldsForTest(), 0);
        QVERIFY(bridge.holdAnimation(&owner,
                                     QUrl::fromLocalFile(path).toString()));
        QVERIFY(!bridge.holdAnimation(&owner, QString()));
        QCOMPARE(bridge.animationHoldsForTest(), 0);
        QVERIFY(!bridge.holdAnimation(nullptr,
                                      QUrl::fromLocalFile(path).toString()));
        QVERIFY(bridge.holdAnimation(&owner,
                                     QUrl::fromLocalFile(path).toString()));
        client.logout();
        QCOMPARE(bridge.animationHoldsForTest(), 0);
    }

    void anAnimationWrittenAgainIsANewUrl()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        const QString mxc = QStringLiteral("mxc://x/again");
        const QString first = writeAnimation(client, bridge, mxc);
        QVERIFY(QFile::remove(first));
        const QString second =
            QUrl(bridge.avatarAnimationSource(mxc, false)).toLocalFile();
        QVERIFY(!second.isEmpty());
        QVERIFY(second != first);
        QVERIFY(QFile::exists(second));
        QCOMPARE(QUrl(bridge.avatarAnimationSource(mxc, false)).toLocalFile(),
                 second);
    }
};

QTEST_GUILESS_MAIN(MediaBridgeTest)
#include "MediaBridgeTest.moc"
