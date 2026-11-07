
#include "app/ForwardController.h"
#include "matrix/MatrixClient.h"
#include "media/MediaBridge.h"

#include <QFile>
#include <QSignalSpy>
#include <QBuffer>
#include <QImage>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    struct Fetch { quint64 opId; QString mediaKey; };
    QList<Fetch> fetches;
    bool rejectFetches = false;

    bool supportsMediaBridge() const override { return true; }
    quint64 fetchMedia(const QString &mediaKey, int,
                       int) override
    {
        if (rejectFetches)
            return 0;
        const quint64 op = nextOp++;
        fetches.append({ op, mediaKey });
        return op;
    }
    quint64 fetchMxcThumbnail(const QString &, int, int) override { return 0; }
    void cancelMediaFetch(quint64) override {}

    void succeed(quint64 opId, const QByteArray &bytes,
                const QString &mime = QStringLiteral("image/png"))
    {
        Q_EMIT mediaReady(opId, QString(), 0, bytes, mime, QString());
    }
    void fail(quint64 opId, const QString &category)
    {
        Q_EMIT mediaFailed(opId, QString(), 0, category);
    }
    void emitLoggedOut() { Q_EMIT loggedOut(); }

    struct TextSend { QString roomId; QString body; };
    struct AttachmentSend {
        QString roomId; QByteArray bytes; QString filename; QString mime;
        int width; int height;
    };
    QList<TextSend> textSends;
    QList<AttachmentSend> attachmentSends;
    quint64 nextSendOp = 1;
    bool rejectNextAttachment = false;

    void sendTextMessage(const QString &roomId, const QString &body) override
    {
        textSends.append({ roomId, body });
    }
    struct ThreadSend { QString roomId; QString rootId; QString body; };
    QList<ThreadSend> threadSends;
    void sendThreadReplyTo(const QString &roomId, const QString &rootId,
                           const QString &, const QString &body) override
    {
        threadSends.append({ roomId, rootId, body });
    }
    QString openRoomId;
    bool roomScopedSupported = true;

    quint64 sendAttachmentBytes(const QString &roomId, const QByteArray &bytes,
                               const QString &filename, const QString &mime,
                               int width, int height,
                               const QString &) override
    {
        if (!openRoomId.isEmpty() && roomId != openRoomId)
            return 0;
        attachmentSends.append({ roomId, bytes, filename, mime, width, height });
        if (rejectNextAttachment) {
            rejectNextAttachment = false;
            return 0;
        }
        return nextSendOp++;
    }

    bool supportsRoomScopedAttachmentSend() const override
    { return roomScopedSupported; }
    bool spoilerSupported = true;
    QList<QString> spoilerSends;
    bool supportsSpoilerMedia() const override { return spoilerSupported; }
    quint64 sendSpoilerMedia(const QString &roomId, const QString &,
                             const QString &, const QByteArray &,
                             const QString &, const QString &, const QString &,
                             int, int, qint64, bool, const QByteArray &, int, int,
                             const QString &) override
    {
        spoilerSends.append(roomId);
        return nextSendOp++;
    }
    quint64 sendAttachmentBytesToRoom(const QString &roomId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime,
                                      int width, int height) override
    {
        attachmentSends.append({ roomId, bytes, filename, mime, width, height });
        if (rejectNextAttachment) {
            rejectNextAttachment = false;
            return 0;
        }
        return nextSendOp++;
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

const QString kRoomA = QStringLiteral("!a:example.org");
const QString kRoomB = QStringLiteral("!b:example.org");
const QString kEventA = QStringLiteral("$eventA");
const QString kEventB = QStringLiteral("$eventB");

static QByteArray realPngBytes()
{
    return QByteArray::fromHex(
        "89504e470d0a1a0a0000000d494844520000000100000001080200000"
        "0907753de0000000c49444154789c63f8cfc0000003010100c9fe92ef"
        "0000000049454e44ae426082");
}

static QByteArray realGifBytes()
{
    return QByteArray::fromHex(
        "47494638396101000100800000000000ffffff21f90401000000"
        "002c00000000010001000002024401003b");
}

QVariantMap textSnapshot(const QString &body = QStringLiteral("hello there"))
{
    QVariantMap m;
    m.insert(QStringLiteral("body"), body);
    m.insert(QStringLiteral("senderDisplayName"), QStringLiteral("Alice"));
    return m;
}

QVariantMap mediaSnapshot(const QString &mediaKey,
                         const QString &filename = QStringLiteral("cat.png"),
                         const QString &mime = QStringLiteral("image/png"))
{
    QVariantMap m;
    m.insert(QStringLiteral("isImage"), true);
    m.insert(QStringLiteral("mediaKey"), mediaKey);
    m.insert(QStringLiteral("mediaFilename"), filename);
    m.insert(QStringLiteral("mediaMimetype"), mime);
    m.insert(QStringLiteral("mediaWidth"), 100);
    m.insert(QStringLiteral("mediaHeight"), 80);
    return m;
}

QVariantMap selectedText(const QString &eventId, const QString &body)
{
    QVariantMap m = textSnapshot(body);
    m.insert(QStringLiteral("eventId"), eventId);
    return m;
}

QVariantMap selectedMedia(const QString &eventId)
{
    QVariantMap m = mediaSnapshot(QStringLiteral("key:") + eventId);
    m.insert(QStringLiteral("eventId"), eventId);
    return m;
}

}

class ForwardMessageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void everyMessageReachesEveryDestination()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.beginSelection(QStringLiteral("!src:example.org"), {
            selectedText(QStringLiteral("$a"), QStringLiteral("first")),
            selectedText(QStringLiteral("$b"), QStringLiteral("second")),
        });
        QVERIFY(fwd.selectionActive());
        fwd.sendSelection({
            QVariantMap{ { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } },
            QVariantMap{ { QStringLiteral("roomId"), QStringLiteral("!y:e.org") } },
        });

        QCOMPARE(client.textSends.size(), 4);
        QCOMPARE(fwd.progressTotal(), 4);
        QCOMPARE(fwd.progressDone(), 4);
        QCOMPARE(fwd.failureCount(), 0);
    }

    void aThreadDestinationSendsIntoThatThread()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.beginSelection(QStringLiteral("!src:example.org"),
                           { selectedText(QStringLiteral("$a"),
                                         QStringLiteral("hello")) });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") },
            { QStringLiteral("threadRootId"), QStringLiteral("$root") },
        } });
        QCOMPARE(client.threadSends.size(), 1);
        QCOMPARE(client.threadSends.at(0).rootId, QStringLiteral("$root"));
        QVERIFY(client.textSends.isEmpty());
    }

    void oneFailureAmongManyIsReportedAsThatPair()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.beginSelection(QStringLiteral("!src:example.org"), {
            selectedText(QStringLiteral("$a"), QStringLiteral("fine")),
            selectedMedia(QStringLiteral("$m")),
        });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });

        QCOMPARE(fwd.progressTotal(), 2);
        QCOMPARE(fwd.progressDone(), 2);
        QCOMPARE(fwd.failureCount(), 1);
        const QVariantMap failure = fwd.failures().at(0).toMap();
        QCOMPARE(failure.value(QStringLiteral("eventId")).toString(),
                 QStringLiteral("$m"));
        QCOMPARE(failure.value(QStringLiteral("roomId")).toString(),
                 QStringLiteral("!x:e.org"));
        QVERIFY(!failure.value(QStringLiteral("message")).toString().isEmpty());
        QCOMPARE(client.textSends.size(), 1);
    }

    void retryResendsOnlyTheFailures()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.beginSelection(QStringLiteral("!src:example.org"), {
            selectedText(QStringLiteral("$a"), QStringLiteral("fine")),
            selectedMedia(QStringLiteral("$m")),
        });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });
        QCOMPARE(client.textSends.size(), 1);
        QCOMPARE(fwd.failureCount(), 1);

        fwd.retryFailures();
        QCOMPARE(fwd.progressTotal(), 1);
        QVERIFY2(client.textSends.size() == 1,
                 "retry resent a message that had already succeeded");
    }

    void contextModeAttributesAndIsNeverTheDefault()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        QCOMPARE(fwd.forwardMode(), QStringLiteral("content"));
        fwd.setForwardMode(QStringLiteral("nonsense"));
        QCOMPARE(fwd.forwardMode(), QStringLiteral("content"));

        QVariantMap snap = selectedText(QStringLiteral("$a"),
                                        QStringLiteral("the body"));
        snap.insert(QStringLiteral("senderName"), QStringLiteral("Ann"));
        snap.insert(QStringLiteral("sourceRoomName"), QStringLiteral("Secret"));
        fwd.beginSelection(QStringLiteral("!src:example.org"), { snap });

        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });
        QVERIFY2(!client.textSends.at(0).body.contains(QStringLiteral("Secret")),
                 "content mode leaked the source room's name");

        client.textSends.clear();
        fwd.setForwardMode(QStringLiteral("context"));
        fwd.beginSelection(QStringLiteral("!src:example.org"), { snap });
        fwd.sendSelection({ QVariantMap{
            { QStringLiteral("roomId"), QStringLiteral("!x:e.org") } } });
        const QString body = client.textSends.at(0).body;
        QVERIFY2(body.contains(QStringLiteral("Ann")), qPrintable(body));
        QVERIFY2(body.contains(QStringLiteral("Secret")), qPrintable(body));
        QVERIFY2(body.contains(QStringLiteral("the body")), qPrintable(body));
    }

    void beginRefusesRedactedEvent()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap = textSnapshot();
        snap.insert(QStringLiteral("redacted"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void beginRefusesLocalEcho()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap = textSnapshot();
        snap.insert(QStringLiteral("isLocalEcho"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void beginRefusesUndecryptableEvent()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap = textSnapshot();
        snap.insert(QStringLiteral("undecryptable"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void bothForwardSnapshotsCarryTheLocationFlag()
    {
        QFile file(QStringLiteral(QML_DIR "/MessageDelegate.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString qml = QString::fromUtf8(file.readAll());
        QCOMPARE(qml.count(QStringLiteral("app.forward.toggleSelected(")), 1);
        QCOMPARE(qml.count(QStringLiteral("app.forward.begin(")), 1);
        QCOMPARE(qml.count(QStringLiteral("isLocation: model.isLocation === true")), 2);
        const int toggle = qml.indexOf(QStringLiteral("app.forward.toggleSelected("));
        const int begin = qml.indexOf(QStringLiteral("app.forward.begin("));
        for (const int at : {toggle, begin}) {
            const int close = qml.indexOf(QStringLiteral("})"), at);
            QVERIFY(close > at);
            QVERIFY2(qml.mid(at, close - at)
                         .contains(QStringLiteral("isLocation: model.isLocation === true")),
                     "a forward snapshot omits isLocation");
        }
    }

    void beginRefusesALocationInsteadOfSendingItAsText()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap = textSnapshot(QStringLiteral("Vilnius"));
        snap.insert(QStringLiteral("isLocation"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(fwd.error().contains(QStringLiteral("Locations")));

        fwd.toggleSelected(QStringLiteral("$loc"), snap);
        QVERIFY(!fwd.isSelected(QStringLiteral("$loc")));
    }

    void beginRefusesEmptyTextBody()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        fwd.begin(kRoomA, kEventA, textSnapshot(QStringLiteral("   ")));

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void beginRefusesMediaWithNoMediaKey()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);

        QVariantMap snap;
        snap.insert(QStringLiteral("isImage"), true);
        fwd.begin(kRoomA, kEventA, snap);

        QVERIFY(!fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void textForwardSendsPlainBodyWithNoRelationThenNavigates()
    {
        FakeClient client;
        ForwardController fwd;
        fwd.setClient(&client);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, textSnapshot(QStringLiteral("hi from A")));
        QVERIFY(fwd.active());

        fwd.forwardTo(kRoomB);

        QCOMPARE(client.textSends.size(), 1);
        QCOMPARE(client.textSends.first().roomId, kRoomB);
        QCOMPARE(client.textSends.first().body, QStringLiteral("hi from A"));
        QCOMPARE(client.attachmentSends.size(), 0);

        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.first().at(0).toString(), kRoomB);
        QVERIFY(!fwd.active());
        QVERIFY(!fwd.busy());
    }

    void mediaForwardRefetchesFreshBytesAndReuploadsThem()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);

        QCOMPARE(client.fetches.size(), 1);
        QCOMPARE(client.fetches.first().mediaKey, QStringLiteral("$mediaX"));
        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(fwd.busy());

        const QByteArray realBytes = realGifBytes();
        client.succeed(client.fetches.first().opId, realBytes);

        QCOMPARE(client.attachmentSends.size(), 1);
        const auto &sent = client.attachmentSends.first();
        QCOMPARE(sent.roomId, kRoomB);
        QCOMPARE(sent.bytes, realBytes);
        QCOMPARE(sent.filename, QStringLiteral("cat.png"));
        QCOMPARE(sent.mime, QStringLiteral("image/gif"));
        QCOMPARE(sent.width, 1);
        QCOMPARE(sent.height, 1);

        QCOMPARE(forwarded.count(), 1);
        QCOMPARE(forwarded.first().at(0).toString(), kRoomB);
        QVERIFY(!fwd.active());
    }

    void aSpoilerIsForwardedWithItsMarkerOrNotAtAll()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        QVariantMap snapshot = mediaSnapshot(QStringLiteral("$mediaX"));
        snapshot.insert(QStringLiteral("mediaSpoiler"), true);
        fwd.begin(kRoomA, kEventA, snapshot);
        fwd.forwardTo(kRoomB);
        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.spoilerSends, QList<QString>{ kRoomB });
        QCOMPARE(client.attachmentSends.size(), 0);

        client.spoilerSupported = false;
        fwd.begin(kRoomA, kEventA, snapshot);
        fwd.forwardTo(kRoomB);
        client.succeed(client.fetches.last().opId, realGifBytes());
        QCOMPARE(client.spoilerSends.size(), 1);
        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(!fwd.error().isEmpty());
    }

    void mediaFetchFailureLeavesDialogOpenWithError()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        client.fail(client.fetches.first().opId, QStringLiteral("network"));

        QCOMPARE(forwarded.count(), 0);
        QVERIFY(fwd.active());
        QVERIFY(!fwd.busy());
        QVERIFY(!fwd.error().isEmpty());
        QCOMPARE(client.attachmentSends.size(), 0);
    }

    void attachmentDispatchFailureLeavesDialogOpenWithError()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        client.rejectNextAttachment = true;
        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        client.succeed(client.fetches.first().opId, realGifBytes());

        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(forwarded.count(), 0);
        QVERIFY(fwd.active());
        QVERIFY(!fwd.error().isEmpty());
    }

    void secondBeginInvalidatesFirstForwardsPendingFetch()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaA")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);
        const quint64 staleOp = client.fetches.first().opId;

        fwd.begin(kRoomA, kEventB, mediaSnapshot(QStringLiteral("$mediaB")));
        fwd.forwardTo(kRoomA);
        QCOMPARE(client.fetches.size(), 2);

        client.succeed(staleOp, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 0);
        QCOMPARE(forwarded.count(), 0);
        QVERIFY(fwd.busy());

        client.succeed(client.fetches.at(1).opId, realPngBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().roomId, kRoomA);
        QCOMPARE(client.attachmentSends.first().bytes, realPngBytes());
        QCOMPARE(forwarded.count(), 1);
    }

    void cancelledForwardIgnoresLateMediaAnswer()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        fwd.cancel();
        QVERIFY(!fwd.active());

        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 0);
        QCOMPARE(forwarded.count(), 0);
    }

    void unrelatedMediaBridgeTrafficIsIgnored()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        bridge.fetchFullForStar(QStringLiteral("$someoneElsesMedia"));
        QCOMPARE(client.fetches.size(), 2);
        client.succeed(client.fetches.at(1).opId, realPngBytes());

        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(fwd.busy());

        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().bytes, realGifBytes());
    }

    void secondForwardToWhileBusyIsANoOp()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);
        QVERIFY(fwd.busy());

        fwd.forwardTo(kRoomA);
        QCOMPARE(client.fetches.size(), 1);

        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().roomId, kRoomB);
    }

    void logoutResetsAnInProgressForward()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaX")));
        fwd.forwardTo(kRoomB);
        QVERIFY(fwd.active());

        client.emitLoggedOut();
        QVERIFY(!fwd.active());
        QVERIFY(!fwd.busy());

        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 0);
    }
    void forwardedFilenameIsSanitized()
    {
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral("../../../etc/passwd")),
                 QStringLiteral("passwd"));
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral("C:\\Windows\\System32\\evil.dll")),
                 QStringLiteral("evil.dll"));
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral(".bashrc")),
                 QStringLiteral("bashrc"));
        QCOMPARE(ForwardController::sanitizedForwardFilename(
                     QStringLiteral("holiday.jpg")),
                 QStringLiteral("holiday.jpg"));
        QVERIFY(ForwardController::sanitizedForwardFilename(
                    QString(400, QLatin1Char('a'))).size() <= 128);
    }

    void unverifiableImageClaimIsRefusedRatherThanReUploaded()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$svg")));
        fwd.forwardTo(kRoomB);
        QCOMPARE(client.fetches.size(), 1);

        client.succeed(client.fetches.first().opId,
                       QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"));

        QCOMPARE(client.attachmentSends.size(), 0);
        QVERIFY(!fwd.error().isEmpty());
        QVERIFY2(fwd.active(),
                 "the dialog must stay open so the refusal is visible");
    }

    void mediaForwardsToARoomWhoseTimelineIsNotOpen()
    {
        FakeClient client;
        client.openRoomId = kRoomA;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);
        QSignalSpy forwarded(&fwd, &ForwardController::forwarded);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaA")));
        fwd.forwardTo(kRoomB);
        client.succeed(client.fetches.first().opId, realGifBytes());

        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().roomId, kRoomB);
        QCOMPARE(forwarded.count(), 1);
        QVERIFY(fwd.error().isEmpty());
    }

    void backendWithoutRoomScopedSendStillForwardsIntoTheOpenRoom()
    {
        FakeClient client;
        client.roomScopedSupported = false;
        client.openRoomId = kRoomA;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$mediaA")));
        fwd.forwardTo(kRoomA);
        client.succeed(client.fetches.first().opId, realGifBytes());
        QCOMPARE(client.attachmentSends.size(), 1);
        QVERIFY(fwd.error().isEmpty());
    }

    void aLargeImageIsNotMistakenForUnsafeContent()
    {
        FakeClient client;
        MediaBridge bridge;
        bridge.setClient(&client);
        ForwardController fwd;
        fwd.setClient(&client);
        fwd.setMediaBridge(&bridge);

        QImage big(5120, 2880, QImage::Format_RGB32);
        big.fill(Qt::blue);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        QVERIFY(big.save(&buf, "PNG"));
        buf.close();

        fwd.begin(kRoomA, kEventA, mediaSnapshot(QStringLiteral("$big")));
        fwd.forwardTo(kRoomB);
        client.succeed(client.fetches.first().opId, png);

        QCOMPARE(client.attachmentSends.size(), 1);
        QCOMPARE(client.attachmentSends.first().mime, QStringLiteral("image/png"));
        QCOMPARE(client.attachmentSends.first().width, 5120);
        QVERIFY2(fwd.error().isEmpty(), "a large PNG is ordinary content");
    }

};

QTEST_MAIN(ForwardMessageTest)
#include "ForwardMessageTest.moc"
