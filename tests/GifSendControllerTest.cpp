// v0.6.1: GIF send pipeline. Drives GifSendController with a fake client
// (subclassing MockMatrixClient) so the download → validate → attachment
// handoff is exercised without network: destination capture/isolation,
// room vs thread routing (real m.thread reply, never a room fallback),
// recents-only-on-success, failure categories, cancel, and mime enforcement.

#include "gif/GifSendController.h"
#include "gif/GifRecentModel.h"
#include "matrix/MockMatrixClient.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QHash>
#include <QImage>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest/QtTest>

namespace {

class FakeSendClient : public MockMatrixClient
{
    Q_OBJECT
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsGifProvider() const override { return true; }

    QString lastDownloadUrl;
    quint64 dlOp = 500;
    quint64 gifDownload(const QString &url) override
    {
        lastDownloadUrl = url;
        return ++dlOp;
    }

    struct Sent {
        QString roomId, rootId, mime, filename;
        bool thread = false;
        QByteArray bytes;
        int w = 0, h = 0;
    };
    QList<Sent> sends;
    bool failSend = false;

    quint64 sendAttachmentBytes(const QString &roomId, const QByteArray &bytes,
                                const QString &filename, const QString &mime,
                                int width, int height) override
    {
        if (failSend) return 0;
        sends.append({ roomId, {}, mime, filename, false, bytes, width, height });
        return 42;
    }
    quint64 sendThreadAttachmentBytes(const QString &roomId,
                                      const QString &rootId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime, int width,
                                      int height) override
    {
        if (failSend) return 0;
        sends.append({ roomId, rootId, mime, filename, true, bytes, width, height });
        return 43;
    }

    void finishDownload(quint64 op, bool ok, const QByteArray &bytes,
                        const QString &mime, int w, int h, const QString &cat)
    {
        Q_EMIT gifDownloadFinished(op, ok, bytes, mime, w, h, bytes.size(), cat);
    }
    void logout() override { MockMatrixClient::logout(); Q_EMIT loggedOut(); }
};

QByteArray realGif() { return QByteArray("GIF89a\x10\x00\x10\x00", 10); }

// 2026-08 media round: a REAL, Qt-encoded PNG (this test target links Qt6::Gui — see
// CMakeLists.txt — unlike gif-starred-store-test, which hand-builds a
// minimal header instead). Proves gif::validateRasterBytes' PNG parser
// against actual encoder output, not only a synthetic fixture.
QByteArray realPng(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::red);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    buf.close();
    return bytes;
}

QVariantMap gifMap(const QString &provider, const QString &id,
                   const QString &host)
{
    QVariantMap m;
    m.insert(QStringLiteral("provider"), provider);
    m.insert(QStringLiteral("gifId"), id);
    m.insert(QStringLiteral("title"), QStringLiteral("t"));
    m.insert(QStringLiteral("gifUrl"),
             QStringLiteral("https://%1/media/%2/giphy.gif").arg(host, id));
    m.insert(QStringLiteral("gifWidth"), 200);
    m.insert(QStringLiteral("gifHeight"), 150);
    return m;
}

} // namespace

class GifSendControllerTest : public QObject
{
    Q_OBJECT

    FakeSendClient *client = nullptr;
    GifSendController *send = nullptr;
    GifRecentModel *recent = nullptr;
    QSettings *store = nullptr;
    // v0.6.6: fake local-starred-file store, keyed by content hash — stands
    // in for GifStarredStore::readBytes without any disk I/O.
    QHash<QString, QByteArray> localFiles;

    void setup()
    {
        store = new QSettings(
            QStringLiteral("/tmp/lightning-gif-send-test.ini"),
            QSettings::IniFormat);
        store->clear();
        client = new FakeSendClient;
        recent = new GifRecentModel(store);
        send = new GifSendController;
        send->setClient(client);
        send->setRecentModel(recent);
        localFiles.clear();
        send->setLocalGifReader([this](const QString &hash) {
            return localFiles.value(hash);
        });
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("LightningTest"));
        QCoreApplication::setApplicationName(QStringLiteral("GifSendTest"));
    }
    void cleanup()
    {
        delete send; send = nullptr;
        delete recent; recent = nullptr;
        delete client; client = nullptr;
        if (store) { store->clear(); delete store; store = nullptr; }
    }

    void roomSendDownloadsThenSends();
    void threadSendUsesThreadPath();
    void destinationIsolationSurvivesRoomSwitch();
    void recentOnlyRecordedOnSuccess();
    void downloadFailureReported();
    void sendFailureReported();
    void nonGifMimeRejected();
    void rejectsNonProviderHost();
    void cancelAllDropsPending();
    void logoutCancels();

    // v0.6.6: local-favorite (client-starred) sends bypass the network
    // download entirely — the "GIF" is a stored file, read by content hash.
    void localFavoriteSendsStoredBytesWithoutDownloading();
    void localFavoriteThreadSendUsesThreadPath();
    void localFavoriteMissingBytesRefusesRatherThanSendingEmpty();
    void localFavoriteCorruptedBytesRefused();
    void localFavoriteNotRecordedIntoRecents();
    void localFavoriteThreadSendWithEmptyRootIdReportsUnavailable();

    // 2026-08 media round: a saved chat image can be PNG/JPEG/WebP now, not only GIF —
    // the local send path must carry the TRUE mime/extension/dimensions the
    // bytes decided (gif::validateRasterBytes), never a hardcoded
    // "image/gif" — while an actual GIF keeps sending exactly as before.
    void localFavoritePngSendCarriesTruthfulMimeAndFilename();
    void localFavoriteGifSendStillCarriesImageGifMime();
};

namespace {
QVariantMap localGifMap(const QString &hash)
{
    QVariantMap m;
    m.insert(QStringLiteral("provider"), QStringLiteral("local"));
    m.insert(QStringLiteral("gifId"), hash);
    m.insert(QStringLiteral("gifWidth"), 64);
    m.insert(QStringLiteral("gifHeight"), 48);
    return m;
}
} // namespace

void GifSendControllerTest::roomSendDownloadsThenSends()
{
    setup();
    QSignalSpy ok(send, &GifSendController::sendSucceeded);
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "a", "media.giphy.com"));
    QVERIFY(client->lastDownloadUrl.endsWith(QStringLiteral("/a/giphy.gif")));
    QCOMPARE(send->activeCount(), 1);
    client->finishDownload(client->dlOp, true, realGif(),
                           QStringLiteral("image/gif"), 200, 150, QString());
    QCOMPARE(client->sends.size(), 1);
    QCOMPARE(client->sends.first().roomId, QStringLiteral("!room:hs"));
    QCOMPARE(client->sends.first().mime, QStringLiteral("image/gif"));
    QVERIFY(!client->sends.first().thread);
    QVERIFY(client->sends.first().filename.endsWith(QStringLiteral(".gif")));
    QCOMPARE(ok.count(), 1);
    QCOMPARE(send->activeCount(), 0);
}

void GifSendControllerTest::threadSendUsesThreadPath()
{
    setup();
    send->sendToThread(QStringLiteral("!room:hs"), QStringLiteral("$root"),
                       gifMap("klipy", "b", "static.klipy.com"));
    client->finishDownload(client->dlOp, true, realGif(),
                           QStringLiteral("image/gif"), 200, 150, QString());
    QCOMPARE(client->sends.size(), 1);
    QVERIFY(client->sends.first().thread);        // thread path, not room
    QCOMPARE(client->sends.first().rootId, QStringLiteral("$root"));
    QCOMPARE(client->sends.first().roomId, QStringLiteral("!room:hs"));
}

void GifSendControllerTest::destinationIsolationSurvivesRoomSwitch()
{
    setup();
    send->sendToRoom(QStringLiteral("!original:hs"),
                     gifMap("giphy", "c", "media.giphy.com"));
    const quint64 op = client->dlOp;
    // A second send to a different room starts before the first completes.
    send->sendToRoom(QStringLiteral("!other:hs"),
                     gifMap("giphy", "d", "media.giphy.com"));
    // The first download completes: it must go to its ORIGINAL captured room.
    client->finishDownload(op, true, realGif(), QStringLiteral("image/gif"),
                           200, 150, QString());
    QCOMPARE(client->sends.size(), 1);
    QCOMPARE(client->sends.first().roomId, QStringLiteral("!original:hs"));
}

void GifSendControllerTest::recentOnlyRecordedOnSuccess()
{
    setup();
    QCOMPARE(recent->count(), 0);
    // A failed download does not record a recent.
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "fail", "media.giphy.com"));
    client->finishDownload(client->dlOp, false, {}, {}, 0, 0,
                           QStringLiteral("network"));
    QCOMPARE(recent->count(), 0);
    // A successful send records exactly one.
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "good", "media.giphy.com"));
    client->finishDownload(client->dlOp, true, realGif(),
                           QStringLiteral("image/gif"), 200, 150, QString());
    QCOMPARE(recent->count(), 1);
    QVERIFY(recent->contains(QStringLiteral("giphy"), QStringLiteral("good")));
}

void GifSendControllerTest::downloadFailureReported()
{
    setup();
    QSignalSpy fail(send, &GifSendController::sendFailed);
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "x", "media.giphy.com"));
    client->finishDownload(client->dlOp, false, {}, {}, 0, 0,
                           QStringLiteral("rate_limited"));
    QCOMPARE(fail.count(), 1);
    QCOMPARE(fail.first().at(0).toString(), QStringLiteral("rate_limited"));
    QVERIFY(client->sends.isEmpty());
}

void GifSendControllerTest::sendFailureReported()
{
    setup();
    client->failSend = true;
    QSignalSpy fail(send, &GifSendController::sendFailed);
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "x", "media.giphy.com"));
    client->finishDownload(client->dlOp, true, realGif(),
                           QStringLiteral("image/gif"), 200, 150, QString());
    QCOMPARE(fail.count(), 1);
    QCOMPARE(recent->count(), 0);         // not recorded on send failure
}

void GifSendControllerTest::nonGifMimeRejected()
{
    setup();
    QSignalSpy fail(send, &GifSendController::sendFailed);
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "x", "media.giphy.com"));
    // A defensively-wrong mime from the bridge is refused.
    client->finishDownload(client->dlOp, true, QByteArray("RIFFxxxxWEBP"),
                           QStringLiteral("image/webp"), 200, 150, QString());
    QCOMPARE(fail.count(), 1);
    QVERIFY(client->sends.isEmpty());
}

void GifSendControllerTest::rejectsNonProviderHost()
{
    setup();
    QSignalSpy fail(send, &GifSendController::sendFailed);
    // A non-provider host never even starts a download.
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "x", "evil.example.com"));
    QCOMPARE(fail.count(), 1);
    QVERIFY(client->lastDownloadUrl.isEmpty());
}

void GifSendControllerTest::cancelAllDropsPending()
{
    setup();
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "x", "media.giphy.com"));
    QCOMPARE(send->activeCount(), 1);
    send->cancelAll();
    QCOMPARE(send->activeCount(), 0);
    // A late download completion for a cancelled send is ignored.
    client->finishDownload(client->dlOp, true, realGif(),
                           QStringLiteral("image/gif"), 200, 150, QString());
    QVERIFY(client->sends.isEmpty());
}

void GifSendControllerTest::logoutCancels()
{
    setup();
    send->sendToRoom(QStringLiteral("!room:hs"),
                     gifMap("giphy", "x", "media.giphy.com"));
    const quint64 op = client->dlOp;
    client->logout();                      // emits loggedOut → cancelAll
    QCOMPARE(send->activeCount(), 0);
    client->finishDownload(op, true, realGif(), QStringLiteral("image/gif"),
                           200, 150, QString());
    QVERIFY(client->sends.isEmpty());      // stale send never routes
}

void GifSendControllerTest::localFavoriteSendsStoredBytesWithoutDownloading()
{
    setup();
    const QString hash = QString(64, QLatin1Char('a'));
    localFiles.insert(hash, realGif());
    QSignalSpy ok(send, &GifSendController::sendSucceeded);

    send->sendToRoom(QStringLiteral("!room:hs"), localGifMap(hash));

    QVERIFY(client->lastDownloadUrl.isEmpty()); // never went through gifDownload()
    QCOMPARE(client->sends.size(), 1);
    QCOMPARE(client->sends.first().roomId, QStringLiteral("!room:hs"));
    QCOMPARE(client->sends.first().bytes, realGif()); // the EXACT stored bytes
    QCOMPARE(client->sends.first().mime, QStringLiteral("image/gif"));
    QVERIFY(!client->sends.first().thread);
    QCOMPARE(ok.count(), 1);
    QCOMPARE(send->activeCount(), 0); // synchronous — never left pending
}

void GifSendControllerTest::localFavoriteThreadSendUsesThreadPath()
{
    setup();
    const QString hash = QString(64, QLatin1Char('b'));
    localFiles.insert(hash, realGif());

    send->sendToThread(QStringLiteral("!room:hs"), QStringLiteral("$root"),
                       localGifMap(hash));

    QCOMPARE(client->sends.size(), 1);
    QVERIFY(client->sends.first().thread);
    QCOMPARE(client->sends.first().rootId, QStringLiteral("$root"));
    QCOMPARE(client->sends.first().bytes, realGif());
}

void GifSendControllerTest::localFavoriteMissingBytesRefusesRatherThanSendingEmpty()
{
    setup();
    QSignalSpy fail(send, &GifSendController::sendFailed);
    // Never populated in localFiles — e.g. unstarred/removed between
    // activation and send.
    send->sendToRoom(QStringLiteral("!room:hs"),
                     localGifMap(QString(64, QLatin1Char('c'))));
    QCOMPARE(fail.count(), 1);
    QVERIFY(client->sends.isEmpty());
}

void GifSendControllerTest::localFavoriteCorruptedBytesRefused()
{
    setup();
    const QString hash = QString(64, QLatin1Char('d'));
    localFiles.insert(hash, QByteArrayLiteral("not a gif at all"));
    QSignalSpy fail(send, &GifSendController::sendFailed);
    send->sendToRoom(QStringLiteral("!room:hs"), localGifMap(hash));
    QCOMPARE(fail.count(), 1);
    QVERIFY(client->sends.isEmpty());
}

void GifSendControllerTest::localFavoriteNotRecordedIntoRecents()
{
    setup();
    const QString hash = QString(64, QLatin1Char('e'));
    localFiles.insert(hash, realGif());
    send->sendToRoom(QStringLiteral("!room:hs"), localGifMap(hash));
    QCOMPARE(client->sends.size(), 1);
    // Recents is a global (not account-scoped) store; a local-starred
    // reference must never cross into it — see GifSendController::startLocal.
    QCOMPARE(recent->count(), 0);
}

void GifSendControllerTest::localFavoriteThreadSendWithEmptyRootIdReportsUnavailable()
{
    // NIT 17: a captured thread destination with no root id was never
    // actually attempted — "unavailable", not "send_failed" (which implies
    // a real send was tried against the SDK and failed).
    setup();
    const QString hash = QString(64, QLatin1Char('f'));
    localFiles.insert(hash, realGif());
    QSignalSpy fail(send, &GifSendController::sendFailed);

    send->sendToThread(QStringLiteral("!room:hs"), QString(), localGifMap(hash));

    QCOMPARE(fail.count(), 1);
    QCOMPARE(fail.first().at(0).toString(), QStringLiteral("unavailable"));
    QVERIFY(client->sends.isEmpty());
}

void GifSendControllerTest::localFavoritePngSendCarriesTruthfulMimeAndFilename()
{
    setup();
    const QString hash = QString(64, QLatin1Char('9'));
    const QByteArray png = realPng(48, 32);
    localFiles.insert(hash, png);
    QSignalSpy ok(send, &GifSendController::sendSucceeded);

    send->sendToRoom(QStringLiteral("!room:hs"), localGifMap(hash));

    QCOMPARE(client->sends.size(), 1);
    QCOMPARE(client->sends.first().mime, QStringLiteral("image/png"));
    QVERIFY(client->sends.first().filename.endsWith(QStringLiteral(".png")));
    QVERIFY(!client->sends.first().filename.endsWith(QStringLiteral(".gif")));
    QCOMPARE(client->sends.first().bytes, png); // exact bytes, never transcoded
    QCOMPARE(client->sends.first().w, 48);
    QCOMPARE(client->sends.first().h, 32);
    QCOMPARE(ok.count(), 1);
}

void GifSendControllerTest::localFavoriteGifSendStillCarriesImageGifMime()
{
    // Belt-and-suspenders alongside localFavoriteSendsStoredBytesWithoutDownloading:
    // the generalized validator must not have changed GIF's own outcome.
    setup();
    const QString hash = QString(64, QLatin1Char('8'));
    localFiles.insert(hash, realGif());

    send->sendToRoom(QStringLiteral("!room:hs"), localGifMap(hash));

    QCOMPARE(client->sends.size(), 1);
    QCOMPARE(client->sends.first().mime, QStringLiteral("image/gif"));
    QVERIFY(client->sends.first().filename.endsWith(QStringLiteral(".gif")));
}

QTEST_MAIN(GifSendControllerTest)
#include "GifSendControllerTest.moc"
