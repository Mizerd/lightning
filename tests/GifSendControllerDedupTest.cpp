// Regression: one user activation of a GIF must produce exactly one send.
// GifPicker.qml's own `activated` one-shot latch (qml/GifPicker.qml) closes
// the common case — a second click/Return landing while the popup is still
// visually closing — but that latch is QML-local, per-picker-instance state.
// It says nothing about any OTHER caller of sendToRoom()/sendToThread(): a
// future call site, a test, or two independently-instantiated pickers (the
// room composer and the thread panel each own one, sharing one
// GifSearchController) racing on the same underlying controller.
//
// This suite drives GifSendController directly — the authoritative backstop
// — and proves: (1) two activations of the SAME gif into the SAME
// destination while the first is still downloading collapse into ONE
// network operation and ONE send; (2) the dedup key is destination-AND-
// identity qualified, so a different room, a different thread root, or a
// different gif is never incorrectly suppressed; (3) the guard only covers
// genuinely in-flight duplicates — a deliberate repeat send AFTER the first
// one has already resolved (succeeded or failed) is never blocked, because
// by then it is no longer "pending" and is ordinary, legitimate user intent.
//
// Mirrors tests/GifSendControllerTest.cpp's FakeSendClient harness (kept
// separate/self-contained per QtTest's one-executable-per-.moc convention
// rather than shared, consistent with that file not being in this
// checkpoint's ownership).
#include "gif/GifSendController.h"
#include "gif/GifRecentModel.h"
#include "matrix/MockMatrixClient.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest/QtTest>

namespace {

class FakeDedupClient : public MockMatrixClient
{
    Q_OBJECT
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsGifProvider() const override { return true; }

    QList<QString> downloadedUrls;
    quint64 dlOp = 700;
    quint64 gifDownload(const QString &url) override
    {
        downloadedUrls.append(url);
        return ++dlOp;
    }

    struct Sent {
        QString roomId, rootId;
        bool thread = false;
    };
    QList<Sent> sends;

    quint64 sendAttachmentBytes(const QString &roomId, const QByteArray &,
                                const QString &, const QString &, int,
                                int) override
    {
        sends.append({ roomId, {}, false });
        return 42;
    }
    quint64 sendThreadAttachmentBytes(const QString &roomId,
                                      const QString &rootId, const QByteArray &,
                                      const QString &, const QString &, int,
                                      int) override
    {
        sends.append({ roomId, rootId, true });
        return 43;
    }

    void finishDownload(quint64 op, bool ok)
    {
        Q_EMIT gifDownloadFinished(op, ok, ok ? realGifBytes() : QByteArray(),
                                   ok ? QStringLiteral("image/gif") : QString(),
                                   200, 150, ok ? 10 : 0,
                                   ok ? QString() : QStringLiteral("network"));
    }

    static QByteArray realGifBytes()
    {
        return QByteArray("GIF89a\x10\x00\x10\x00", 10);
    }
};

QVariantMap gifMap(const QString &provider, const QString &id)
{
    QVariantMap m;
    m.insert(QStringLiteral("provider"), provider);
    m.insert(QStringLiteral("gifId"), id);
    m.insert(QStringLiteral("title"), QStringLiteral("t"));
    m.insert(QStringLiteral("gifUrl"),
             QStringLiteral("https://media.giphy.com/media/%1/giphy.gif")
                 .arg(id));
    m.insert(QStringLiteral("gifWidth"), 200);
    m.insert(QStringLiteral("gifHeight"), 150);
    return m;
}

} // namespace

class GifSendControllerDedupTest : public QObject
{
    Q_OBJECT

    FakeDedupClient *client = nullptr;
    GifSendController *send = nullptr;
    GifRecentModel *recent = nullptr;
    QSettings *store = nullptr;

    void setup()
    {
        store = new QSettings(
            QStringLiteral("/tmp/lightning-gif-send-dedup-test.ini"),
            QSettings::IniFormat);
        store->clear();
        client = new FakeDedupClient;
        recent = new GifRecentModel(store);
        send = new GifSendController;
        send->setClient(client);
        send->setRecentModel(recent);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("LightningTest"));
        QCoreApplication::setApplicationName(
            QStringLiteral("GifSendDedupTest"));
    }
    void cleanup()
    {
        delete send; send = nullptr;
        delete recent; recent = nullptr;
        delete client; client = nullptr;
        if (store) { store->clear(); delete store; store = nullptr; }
    }

    void identicalRoomActivationWhileFirstInFlightSendsOnce();
    void identicalThreadActivationWhileFirstInFlightSendsOnce();
    void differentRoomNotDeduplicated();
    void differentThreadRootNotDeduplicated();
    void differentGifNotDeduplicated();
    void repeatAfterSuccessIsNotBlocked();
    void repeatAfterFailureIsNotBlocked();
};

void GifSendControllerDedupTest::identicalRoomActivationWhileFirstInFlightSendsOnce()
{
    setup();
    QSignalSpy started(send, &GifSendController::sendStarted);
    QSignalSpy ok(send, &GifSendController::sendSucceeded);
    // Two activations of the exact same GIF into the exact same room, the
    // second landing before the first's download completes — exactly the
    // "double click before the popup finishes closing" race.
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    QCOMPARE(client->downloadedUrls.size(), 1);   // one network op, not two
    QCOMPARE(send->activeCount(), 1);
    QCOMPARE(started.count(), 1);
    client->finishDownload(client->dlOp, true);
    QCOMPARE(client->sends.size(), 1);             // one Matrix send, not two
    QCOMPARE(ok.count(), 1);
    QCOMPARE(recent->count(), 1);                  // recorded once
}

void GifSendControllerDedupTest::identicalThreadActivationWhileFirstInFlightSendsOnce()
{
    setup();
    send->sendToThread(QStringLiteral("!room:hs"), QStringLiteral("$root"),
                       gifMap("klipy", "b"));
    send->sendToThread(QStringLiteral("!room:hs"), QStringLiteral("$root"),
                       gifMap("klipy", "b"));
    QCOMPARE(client->downloadedUrls.size(), 1);
    QCOMPARE(send->activeCount(), 1);
    client->finishDownload(client->dlOp, true);
    QCOMPARE(client->sends.size(), 1);
    QVERIFY(client->sends.first().thread);
    QCOMPARE(client->sends.first().rootId, QStringLiteral("$root"));
}

void GifSendControllerDedupTest::differentRoomNotDeduplicated()
{
    setup();
    // Same GIF, two DIFFERENT rooms — the dedup key is destination-
    // qualified, so both must proceed as independent sends.
    send->sendToRoom(QStringLiteral("!roomA:hs"), gifMap("giphy", "a"));
    send->sendToRoom(QStringLiteral("!roomB:hs"), gifMap("giphy", "a"));
    QCOMPARE(client->downloadedUrls.size(), 2);
    QCOMPARE(send->activeCount(), 2);
}

void GifSendControllerDedupTest::differentThreadRootNotDeduplicated()
{
    setup();
    // Same room, same GIF, two DIFFERENT thread roots — still two distinct
    // destinations (and distinct from an ordinary room send of the same
    // GIF), never collapsed.
    send->sendToThread(QStringLiteral("!room:hs"), QStringLiteral("$rootA"),
                       gifMap("giphy", "a"));
    send->sendToThread(QStringLiteral("!room:hs"), QStringLiteral("$rootB"),
                       gifMap("giphy", "a"));
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    QCOMPARE(client->downloadedUrls.size(), 3);
    QCOMPARE(send->activeCount(), 3);
}

void GifSendControllerDedupTest::differentGifNotDeduplicated()
{
    setup();
    // Same room, two DIFFERENT gifs — never collapsed.
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "b"));
    QCOMPARE(client->downloadedUrls.size(), 2);
    QCOMPARE(send->activeCount(), 2);
}

void GifSendControllerDedupTest::repeatAfterSuccessIsNotBlocked()
{
    setup();
    // The guard only ever covers a genuinely IN-FLIGHT duplicate. Once the
    // first send has resolved, a deliberate repeat of the identical GIF to
    // the identical destination is ordinary user intent and must proceed.
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    client->finishDownload(client->dlOp, true);
    QCOMPARE(client->sends.size(), 1);
    QCOMPARE(send->activeCount(), 0);

    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    QCOMPARE(client->downloadedUrls.size(), 2);
    QCOMPARE(send->activeCount(), 1);
    client->finishDownload(client->dlOp, true);
    QCOMPARE(client->sends.size(), 2);
}

void GifSendControllerDedupTest::repeatAfterFailureIsNotBlocked()
{
    setup();
    QSignalSpy fail(send, &GifSendController::sendFailed);
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    client->finishDownload(client->dlOp, false);
    QCOMPARE(fail.count(), 1);
    QCOMPARE(send->activeCount(), 0);

    // Retrying the identical GIF/destination after a failure must not be
    // silently swallowed as a "duplicate" — the failed attempt is no
    // longer pending.
    send->sendToRoom(QStringLiteral("!room:hs"), gifMap("giphy", "a"));
    QCOMPARE(client->downloadedUrls.size(), 2);
    QCOMPARE(send->activeCount(), 1);
}

QTEST_MAIN(GifSendControllerDedupTest)
#include "GifSendControllerDedupTest.moc"
