// v0.5.9: attachment pipeline tests — file validation (directories,
// unreadable, empty, oversized, MIME from content), queue state machine
// (dispatch, success removal, failure + retry), and lifecycle safety
// (room switch and sign-out clear the tray; stale completions ignored).

#include "matrix/MatrixClient.h"
#include "models/MessageComposer.h"

#include <QBuffer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    quint64 nextOp = 1;
    quint64 lastOpId = 0;
    int fileSends = 0;
    int byteSends = 0;
    QString lastMime;
    QString lastFilename;
    qint64 serverLimit = 0;
    bool rejectSends = false;

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

    bool supportsAttachmentSend() const override { return true; }
    qint64 maxUploadSize() const override { return serverLimit; }
    quint64 sendAttachment(const QString &, const QString &,
                           const QString &mime, const QString &,
                           int, int, bool) override
    {
        if (rejectSends)
            return 0;
        ++fileSends;
        lastMime = mime;
        lastOpId = nextOp++;
        return lastOpId;
    }
    // v0.7 video round: records exactly what the send path declared, so a
    // test can prove the poster and the video geometry reached the client
    // rather than merely that "a send happened".
    int videoSends = 0;
    QByteArray lastThumbnail;
    int lastThumbWidth = 0;
    int lastThumbHeight = 0;
    int lastVideoWidth = 0;
    int lastVideoHeight = 0;
    qint64 lastDurationMs = 0;
    quint64 sendVideo(const QString &, const QString &, const QString &mime,
                      const QString &, int width, int height,
                      qint64 durationMs, const QByteArray &thumbnail,
                      int thumbnailWidth, int thumbnailHeight) override
    {
        if (rejectSends)
            return 0;
        ++videoSends;
        lastMime = mime;
        lastVideoWidth = width;
        lastVideoHeight = height;
        lastDurationMs = durationMs;
        lastThumbnail = thumbnail;
        lastThumbWidth = thumbnailWidth;
        lastThumbHeight = thumbnailHeight;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 sendAttachmentBytes(const QString &, const QByteArray &,
                                const QString &filename, const QString &mime,
                                int, int) override
    {
        if (rejectSends)
            return 0;
        ++byteSends;
        lastFilename = filename;
        lastMime = mime;
        lastOpId = nextOp++;
        return lastOpId;
    }
    // v0.7 thread parity: records the voice send so a test can prove the
    // preflight ran BEFORE the client was asked to send anything.
    int voiceSends = 0;
    QString lastVoiceRoom;
    quint64 sendVoiceMessage(const QString &roomId, const QString &,
                             const QString &, qint64,
                             const QList<int> &) override
    {
        if (rejectSends)
            return 0;
        ++voiceSends;
        lastVoiceRoom = roomId;
        lastOpId = nextOp++;
        return lastOpId;
    }
};

QString writeFile(const QTemporaryDir &dir, const QString &name,
                  const QByteArray &content)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    file.close();
    return path;
}

// Minimal valid 1x1 PNG.
QByteArray tinyPng()
{
    QByteArray png = QByteArray::fromHex(
        "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
        "0000000d4944415478da63fcff9fa10e0003030101c9fe92ef0000000049454e44"
        "ae426082");
    return png;
}

// Minimal ISO base-media (MP4) header: an `ftyp` box with the `isom` brand.
// Enough for MIME detection; deliberately NOT a decodable clip — the poster
// hook stands in for the decoder in every test here.
QByteArray tinyMp4Header()
{
    return QByteArray::fromHex(
        "000000206674797069736f6d0000020069736f6d69736f32617663316d703431");
}

// Minimal valid 2x1 JPEG, standing in for an extracted poster frame.
QByteArray tinyJpeg()
{
    QImage image(2, 1, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 80);
    return bytes;
}

} // namespace

class AttachmentQueueTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void rejectsDirectoriesEmptyAndMissing()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        composer.addAttachment(QUrl::fromLocalFile(dir.path())); // directory
        QCOMPARE(rejected.count(), 1);

        const QString empty = writeFile(dir, QStringLiteral("empty.bin"), {});
        composer.addAttachment(QUrl::fromLocalFile(empty)); // zero bytes
        QCOMPARE(rejected.count(), 2);

        composer.addAttachment(
            QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.bin"))));
        QCOMPARE(rejected.count(), 3);

        composer.addAttachment(QUrl(QStringLiteral("https://example.org/x")));
        QCOMPARE(rejected.count(), 4);

        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    void enforcesServerUploadLimit()
    {
        FakeClient client;
        client.serverLimit = 8; // absurdly small, for the test
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString big =
            writeFile(dir, QStringLiteral("big.bin"), QByteArray(64, 'x'));
        composer.addAttachment(QUrl::fromLocalFile(big));
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    // An UNKNOWN server limit (0 — not advertised, not answered yet, or the
    // lookup failed) must not reject anything locally. Before this round a
    // fabricated 100 MiB ceiling stood in, so a 120 MiB file was refused
    // even when the server would have accepted it. Fails on the old code:
    // this file is deliberately larger than that former ceiling.
    void unknownServerLimitDoesNotRejectLocally()
    {
        FakeClient client;
        client.serverLimit = 0;   // unknown
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        // 101 MiB: over the removed 100 MiB fallback, under nothing real.
        QFile f(dir.filePath(QStringLiteral("huge.bin")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.resize(101ll * 1024 * 1024));
        f.close();

        composer.addAttachment(QUrl::fromLocalFile(f.fileName()));
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    // m.upload.size is the largest ACCEPTED payload, so exactly-at-limit
    // must pass while one byte more must not.
    void exactlyAtServerLimitIsAllowed()
    {
        FakeClient client;
        client.serverLimit = 64;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString exact =
            writeFile(dir, QStringLiteral("exact.bin"), QByteArray(64, 'x'));
        composer.addAttachment(QUrl::fromLocalFile(exact));
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);

        const QString over =
            writeFile(dir, QStringLiteral("over.bin"), QByteArray(65, 'x'));
        composer.addAttachment(QUrl::fromLocalFile(over));
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    // Voice messages had NO preflight at all: an oversized recording went
    // straight to an upload the server would refuse. Fails on the old code,
    // where voiceSends would be 1.
    void voiceMessageIsPreflightedAgainstServerLimit()
    {
        FakeClient client;
        client.serverLimit = 16;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("voice.ogg"), QByteArray(64, 'v'));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 1200,
                                  QVariantList{});
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(client.voiceSends, 0);
        // The refused recording is reclaimed, never orphaned on disk.
        QVERIFY(!QFile::exists(rec));
    }

    // A voice send that fails AFTER the user has switched rooms must not
    // surface over the room they are now looking at. Fails on the old code,
    // which discarded the roomId (Q_UNUSED) and emitted unconditionally.
    void lateVoiceFailureDoesNotBleedIntoAnotherRoom()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!a:example.org"));

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("voice.ogg"), QByteArray(8, 'v'));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 900,
                                  QVariantList{});
        QCOMPARE(client.voiceSends, 1);
        const quint64 op = client.lastOpId;

        // The user moves to another room before the upload resolves.
        composer.setRoomId(QStringLiteral("!b:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        Q_EMIT client.attachmentQueueFinished(op, QStringLiteral("!a:example.org"),
                                              false, QString());
        QCOMPARE(rejected.count(), 0);
        // Cleanup is unconditional even though reporting was suppressed.
        QVERIFY(!QFile::exists(rec));
    }

    // The same failure, with the user still in the originating room, MUST
    // be reported — proving the scoping suppresses the wrong context only,
    // not the notice itself.
    void voiceFailureInCurrentRoomIsStillReported()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!a:example.org"));

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("voice.ogg"), QByteArray(8, 'v'));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 900,
                                  QVariantList{});
        const quint64 op = client.lastOpId;

        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        Q_EMIT client.attachmentQueueFinished(op, QStringLiteral("!a:example.org"),
                                              false, QString());
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!QFile::exists(rec));
    }

    void detectsMimeFromContentNotExtension()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        // PNG bytes labelled .txt: content wins.
        const QString disguised =
            writeFile(dir, QStringLiteral("actually-a-png.txt"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(disguised));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        const QModelIndex idx = composer.attachments()->index(0, 0);
        QCOMPARE(composer.attachments()
                     ->data(idx, AttachmentQueueModel::MimeRole).toString(),
                 QStringLiteral("image/png"));
        QVERIFY(composer.attachments()
                    ->data(idx, AttachmentQueueModel::IsImageRole).toBool());
    }

    void duplicatePathIsRejected()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(rejected.count(), 1);
    }

    void sendDispatchesThenRemovesOnSuccess()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("photo.png"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QVERIFY(composer.canSend()); // attachments alone are sendable

        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(composer.attachments()->rowCount(), 1); // dispatching

        // Success: the SDK local echo owns it now; tray entry leaves.
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});
        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    void failedDispatchIsRetryable()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.fileSends, 1);

        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), false,
            QStringLiteral("rejected"));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        const QModelIndex idx = composer.attachments()->index(0, 0);
        QCOMPARE(composer.attachments()
                     ->data(idx, AttachmentQueueModel::StateRole).toString(),
                 QStringLiteral("failed"));

        // Retry re-queues and a new send dispatches it again.
        composer.attachments()->retryAt(0);
        composer.send();
        QCOMPARE(client.fileSends, 2);
    }

    // ── v0.7 video round: send-side posters ──────────────────────────────
    // A video is postered from the file the user picked before it is
    // dispatched, and the poster plus the geometry and duration the decoder
    // reported are what the send path declares on the Matrix event.
    void videoSendCarriesExtractedPoster()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &path) {
                QVERIFY(!path.isEmpty());
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::MimeRole)
                     .toString(),
                 QStringLiteral("video/mp4"));
        // Extraction started on add, not on send.
        QVERIFY(!capturedTag.isEmpty());

        // Pressing send while the poster is still decoding must NOT
        // dispatch — a video without its poster is exactly the event this
        // round exists to stop sending.
        composer.send();
        QCOMPARE(client.videoSends, 0);
        QCOMPARE(client.fileSends, 0);

        const QByteArray poster = tinyJpeg();
        QVERIFY(!poster.isEmpty());
        composer.attachments()->applyPoster(capturedTag, poster, QSize(320, 180),
                                            QSize(1920, 1080), 4200);

        QCOMPARE(client.videoSends, 1);
        QCOMPARE(client.fileSends, 0); // never the plain attachment path
        QCOMPARE(client.lastMime, QStringLiteral("video/mp4"));
        QCOMPARE(client.lastThumbnail, poster);
        QCOMPARE(client.lastThumbWidth, 320);
        QCOMPARE(client.lastThumbHeight, 180);
        // The decoded frame is the only honest source of the video's own
        // dimensions on the send side.
        QCOMPARE(client.lastVideoWidth, 1920);
        QCOMPARE(client.lastVideoHeight, 1080);
        QCOMPARE(client.lastDurationMs, 4200);
        // 16:9 in, 16:9 out — the poster never distorts the frame.
        QCOMPARE(client.lastThumbWidth * client.lastVideoHeight,
                 client.lastThumbHeight * client.lastVideoWidth);
    }

    // Thumbnail extraction failing is not send failure: the video goes out
    // without a poster rather than being stuck in the tray forever.
    void videoSendsWithoutPosterWhenExtractionFails()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &) {
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("broken.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.videoSends, 0);

        // The decoder gave up: empty poster, no geometry, no duration.
        composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 0);

        QCOMPARE(client.videoSends, 1);
        QVERIFY(client.lastThumbnail.isEmpty());
        QCOMPARE(client.lastThumbWidth, 0);
        QCOMPARE(client.lastThumbHeight, 0);
        QCOMPARE(client.lastVideoWidth, 0);
        QCOMPARE(client.lastVideoHeight, 0);
        QCOMPARE(client.lastDurationMs, 0);
    }

    // A poster arriving with nobody waiting on it must not send anything,
    // and a second callback for the same job must not send twice.
    void posterWithoutSendRequestDoesNotDispatch()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QString capturedTag;
        composer.attachments()->setPosterRequestHook(
            [&capturedTag](const QString &tag, const QString &) {
                capturedTag = tag;
            });

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));

        // Poster resolves before the user ever pressed send.
        composer.attachments()->applyPoster(capturedTag, tinyJpeg(),
                                            QSize(64, 36), QSize(640, 360), 1000);
        QCOMPARE(client.videoSends, 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);

        composer.send();
        QCOMPARE(client.videoSends, 1);

        // A duplicate callback for a job that already resolved is inert.
        composer.attachments()->applyPoster(capturedTag, tinyJpeg(),
                                            QSize(64, 36), QSize(640, 360), 1000);
        QCOMPARE(client.videoSends, 1);
    }

    // An unknown tag (the entry was removed while decoding) is ignored.
    void posterForRemovedEntryIsIgnored()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.attachments()->setPosterRequestHook(
            [](const QString &, const QString &) {});

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header());
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.attachments()->removeAt(0);
        QCOMPARE(composer.attachments()->rowCount(), 0);

        composer.attachments()->applyPoster(QStringLiteral("send:1"), tinyJpeg(),
                                            QSize(64, 36), QSize(640, 360), 1000);
        QCOMPARE(client.videoSends, 0);
    }

    // Non-video attachments are untouched by the poster machinery: no
    // extraction is requested and they still take the plain send path.
    void nonVideoAttachmentsNeverRequestAPoster()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        int posterRequests = 0;
        composer.attachments()->setPosterRequestHook(
            [&posterRequests](const QString &, const QString &) {
                ++posterRequests;
            });

        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'))));
        QCOMPARE(posterRequests, 0);

        composer.send();
        QCOMPARE(posterRequests, 0);
        QCOMPARE(client.fileSends, 2);
        QCOMPARE(client.videoSends, 0);
    }

    void staleCompletionIsIgnored()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();

        // A completion with an unknown op id must not touch the tray.
        Q_EMIT client.attachmentQueueFinished(
            9999, QStringLiteral("!room:example.org"), true, {});
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    void roomSwitchAndLogoutClearQueue()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!a:example.org"));

        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);

        composer.setRoomId(QStringLiteral("!b:example.org"));
        QCOMPARE(composer.attachments()->rowCount(), 0);

        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        client.logout();
        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    void pastedImageDataQueuesWithoutTempFile()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        // Direct in-memory path used by clipboard paste.
        const QString reason = composer.attachments()->addImageData(
            tinyPng(), QStringLiteral("image/png"), 1, 1);
        QVERIFY(reason.isEmpty());
        QCOMPARE(composer.attachments()->rowCount(), 1);

        composer.send();
        QCOMPARE(client.byteSends, 1);
        QCOMPARE(client.lastFilename, QStringLiteral("pasted-image.png"));
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
    }
};

QTEST_GUILESS_MAIN(AttachmentQueueTest)
#include "AttachmentQueueTest.moc"
