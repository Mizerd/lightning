// Attachment pipeline: file validation (directories, unreadable, empty,
// oversized, MIME from content), the queue state machine (dispatch, success
// removal, failure and retry), and lifecycle safety (room switch and sign-out
// clear the tray; stale completions are ignored).

#include "matrix/MatrixClient.h"
#include "media/ImageFormatSupport.h"
#include "media/ImageMetadata.h"
#include "media/StagedImageStore.h"
#include "media/SvgRasterJob.h"
#include "models/MessageComposer.h"

#include <QBuffer>
#include <QImage>
#include <QMimeData>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest/QtTest>

#include <memory>
#include <tuple>

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
    QString userId = QStringLiteral("@me:example.org");
    QString currentUserId() const override { return userId; }
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
    // Shares `lastDurationMs` with the video path: what matters is the
    // duration the send declared, so players do not show "0:00".
    QString lastFilePath;
    int lastFileWidth = 0;
    int lastFileHeight = 0;
    quint64 sendAttachment(const QString &, const QString &path,
                           const QString &mime, const QString &caption,
                           int width, int height, bool, qint64 durationMs,
                           const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++fileSends;
        lastFilePath = path;
        lastFileWidth = width;
        lastFileHeight = height;
        lastReplyTo = replyTo;
        lastCaption = caption;
        lastMime = mime;
        lastDurationMs = durationMs;
        lastOpId = nextOp++;
        return lastOpId;
    }
    // The reply target the most recent attachment send of ANY kind carried;
    // "" for a plain send. Proves the reply survives every send path.
    QString lastReplyTo;
    QString lastCaption;
    int threadFileSends = 0;
    QString lastThreadRoot;
    quint64 sendThreadAttachment(const QString &, const QString &root,
                                 const QString &path, const QString &mime,
                                 const QString &caption, int width, int height,
                                 bool, qint64, const QString &replyTo) override
    {
        ++threadFileSends;
        lastThreadRoot = root;
        lastFilePath = path;
        lastMime = mime;
        lastCaption = caption;
        lastFileWidth = width;
        lastFileHeight = height;
        lastReplyTo = replyTo;
        lastOpId = nextOp++;
        return lastOpId;
    }
    // Records what the video send path declared, so a test can prove the
    // poster and geometry reached the client.
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
                      int thumbnailWidth, int thumbnailHeight,
                      const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++videoSends;
        lastReplyTo = replyTo;
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
    // Records the still-image-with-thumbnail path (an SVG's PNG preview).
    int imageThumbSends = 0;
    int lastImageWidth = 0;
    int lastImageHeight = 0;
    quint64 sendImageWithThumbnail(const QString &, const QString &,
                                   const QString &mime, const QString &,
                                   int width, int height,
                                   const QByteArray &thumbnail,
                                   int thumbnailWidth,
                                   int thumbnailHeight,
                                   const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++imageThumbSends;
        lastReplyTo = replyTo;
        lastMime = mime;
        lastImageWidth = width;
        lastImageHeight = height;
        lastThumbnail = thumbnail;
        lastThumbWidth = thumbnailWidth;
        lastThumbHeight = thumbnailHeight;
        lastOpId = nextOp++;
        return lastOpId;
    }
    QByteArray lastBytes;
    quint64 sendAttachmentBytes(const QString &, const QByteArray &bytes,
                                const QString &filename, const QString &mime,
                                int, int, const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++byteSends;
        lastBytes = bytes;
        lastReplyTo = replyTo;
        lastFilename = filename;
        lastMime = mime;
        lastOpId = nextOp++;
        return lastOpId;
    }
    // Records the voice send so a test can prove the preflight ran first.
    int voiceSends = 0;
    QString lastVoiceRoom;
    quint64 sendVoiceMessage(const QString &roomId, const QString &,
                             const QString &, qint64,
                             const QList<int> &,
                             const QString &replyTo) override
    {
        if (rejectSends)
            return 0;
        ++voiceSends;
        lastReplyTo = replyTo;
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

// A PNG that actually decodes (tinyPng() only has to sniff as one), for the
// image-edit cases that render it.
QByteArray decodablePng()
{
    QImage image(4, 3, QImage::Format_ARGB32);
    image.fill(QColor(20, 120, 220, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// The first bytes of a Matroska file: the EBML header with DocType
// "matroska". The shared MIME database proves only the CONTAINER from these
// (application/x-matroska); whether it holds a picture is in the tracks.
QByteArray tinyMatroskaHeader()
{
    return QByteArray::fromHex(
               "1a45dfa39f428681014282886d6174726f736b61")
        + QByteArray(64, '\0');
}

// An ASF header object GUID (WMV, WMA): proven as application/vnd.ms-asf.
QByteArray tinyAsfHeader()
{
    return QByteArray::fromHex("3026b2758e66cf11a6d900aa0062ce6c")
        + QByteArray(64, '\0');
}

// An ISO base-media header whose brand (avc1) the shared MIME database does
// not know, so the content proves nothing and the extension decides.
QByteArray unknownBrandMp4Header()
{
    return QByteArray::fromHex(
               "000000186674797061766331000000006176633169736f6d")
        + QByteArray(64, '\0');
}

// Minimal ISO base-media (MP4) header: an `ftyp` box with the `isom` brand.
// Enough for MIME detection, not a decodable clip; the poster hook stands in
// for the decoder.
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

// The "coordinates" a GPS-tagged test photo carries.
const QByteArray kGpsMarker(24, '\xAB');

// A real JPEG with an Exif block (orientation 1 and a GPS IFD) right after SOI,
// the way a phone writes it.
QByteArray jpegWithGps()
{
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 85);

    QByteArray t("MM\0*\0\0\0\x08", 8);
    t.append("\0\2", 2);
    t.append("\x01\x12\0\3\0\0\0\1\0\1\0\0", 12);
    t.append("\x88\x25\0\4\0\0\0\1\0\0\0\x26", 12);
    t.append("\0\0\0\0", 4);
    t.append("\0\1", 2);
    t.append("\0\2\0\5\0\0\0\3\0\0\0\x38", 12);
    t.append("\0\0\0\0", 4);
    t.append(kGpsMarker);
    QByteArray payload = QByteArray("Exif\0\0", 6) + t;
    QByteArray segment("\xFF\xE1", 2);
    const quint16 len = quint16(payload.size() + 2);
    segment.append(char(len >> 8));
    segment.append(char(len & 0xFF));
    segment.append(payload);
    jpeg.insert(2, segment);
    return jpeg;
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A plain 1600x1200 SVG: a red square filling the canvas.
QByteArray redSquareSvg()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1600\" "
        "height=\"1200\" viewBox=\"0 0 1600 1200\">"
        "<rect x=\"0\" y=\"0\" width=\"1600\" height=\"1200\" fill=\"#ff0000\"/>"
        "</svg>");
}

// An SVG that asks QtSvg to draw a file from the local disk into the
// thumbnail, which would then be uploaded.
QByteArray svgReadingALocalFile(const QString &path)
{
    return QByteArrayLiteral(
               "<svg xmlns=\"http://www.w3.org/2000/svg\" "
               "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"64\" "
               "height=\"64\"><image width=\"64\" height=\"64\" xlink:href=\"")
        + QUrl::fromLocalFile(path).toEncoded()
        + QByteArrayLiteral("\"/></svg>");
}

} // namespace

class AttachmentQueueTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void imageEditUsesSessionScratchWriter()
    {
        const QString sourcePath = QFileInfo(QString::fromUtf8(__FILE__)).dir()
            .absoluteFilePath(QStringLiteral("../src/models/AttachmentQueueModel.cpp"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray code = source.readAll();
        QVERIFY(code.contains("lightning::scratch::ScratchFile file(path)"));
        QVERIFY(!code.contains("QSaveFile file(path)"));
    }

    void editedImagePreviewIsExactlyTheSentPngAndOriginalSurvives()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QByteArray original = jpegWithGps();
        const QString path = writeFile(dir, QStringLiteral("holiday.jpg"), original);
        composer.addAttachment(QUrl::fromLocalFile(path));
        auto *queue = composer.attachments();
        const QVariantList stroke{QVariantMap{{QStringLiteral("x"), 0.1},
                                             {QStringLiteral("y"), 0.1}},
                                  QVariantMap{{QStringLiteral("x"), 0.9},
                                             {QStringLiteral("y"), 0.9}}};
        const QVariantMap preview = queue->renderImageEdit(
            0, 0.0, 0.0, 0.5, 1.0, 1, true, QVariantList{stroke});
        QVERIFY(preview.contains(QStringLiteral("url")));
        const QString previewPath = preview.value(QStringLiteral("url"))
                                        .toUrl().toLocalFile();
        const QByteArray previewBytes = readAll(previewPath);
        QVERIFY(previewBytes.startsWith("\x89PNG\r\n\x1a\n"));
        QVERIFY(!previewBytes.contains("Exif"));
        QVERIFY(!previewBytes.contains(kGpsMarker));
        QImage edited;
        QVERIFY(edited.loadFromData(previewBytes));
        QCOMPARE(edited.size(), QSize(8, 16));
        QVERIFY(queue->commitImageEdit(0));
        QCOMPARE(queue->entries().at(0).mime, QStringLiteral("image/png"));
        QCOMPARE(queue->entries().at(0).fileName, QStringLiteral("holiday.png"));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
        QCOMPARE(client.lastFileWidth, 16);
        QCOMPARE(client.lastFileHeight, 8);
        QCOMPARE(readAll(client.lastFilePath), previewBytes);
        QCOMPARE(readAll(path), original);
    }

    void cancellingImageEditKeepsTheOriginalQueued()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("photo.png"), decodablePng());
        composer.addAttachment(QUrl::fromLocalFile(path));
        auto *queue = composer.attachments();
        const QVariantMap preview = queue->renderImageEdit(
            0, 0, 0, 1, 1, 0, false, {});
        const QString previewPath = preview.value(QStringLiteral("url"))
                                        .toUrl().toLocalFile();
        QVERIFY(QFileInfo::exists(previewPath));
        queue->cancelImageEdit(0);
        QVERIFY(!QFileInfo::exists(previewPath));
        QCOMPARE(queue->entries().at(0).localPath, path);
        QCOMPARE(readAll(path), decodablePng());
    }

    void cropAfterRotationUsesDisplayedEdges()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QImage source(4, 4, QImage::Format_ARGB32);
        source.fill(Qt::blue);
        for (int y = 0; y < 4; ++y)
            source.setPixelColor(3, y, Qt::red);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "PNG"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("edges.png"), bytes)));
        auto *queue = composer.attachments();
        const QVariantMap preview = queue->renderImageEdit(
            0, 0, 0, 1, 0.5, 1, false, {});
        QImage result(preview.value(QStringLiteral("url")).toUrl().toLocalFile());
        QCOMPARE(result.size(), QSize(4, 2));
        // After a clockwise turn the red source right edge is the visible
        // bottom edge. Bottom trims those rows from the displayed image.
        for (int y = 0; y < result.height(); ++y)
            for (int x = 0; x < result.width(); ++x)
                QCOMPARE(result.pixelColor(x, y), QColor(Qt::blue));
    }

    void editedPngDropsEmbeddedTextButKeepsTransparency()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QImage source(8, 8, QImage::Format_ARGB32);
        source.fill(Qt::transparent);
        source.setPixelColor(3, 3, Qt::red);
        source.setText(QStringLiteral("Location"),
                       QStringLiteral("private location marker"));
        QByteArray encoded;
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "PNG"));
        buffer.close();
        QVERIFY(encoded.contains("private location marker"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), encoded)));
        auto *queue = composer.attachments();
        const QVariantMap preview = queue->renderImageEdit(
            0, 0, 0, 1, 1, 0, false, {});
        const QByteArray sent = readAll(preview.value(QStringLiteral("url"))
                                            .toUrl().toLocalFile());
        QVERIFY(!sent.contains("private location marker"));
        QImage decoded;
        QVERIFY(decoded.loadFromData(sent));
        QCOMPARE(decoded.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(decoded.pixelColor(3, 3), QColor(Qt::red));
    }

    void editedImageKeepsThreadCaptionAndReplyRouting()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setThreadTargetRootId(QStringLiteral("$root:example.org"));
        composer.setSendTextAsCaption(true);
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), decodablePng())));
        auto *queue = composer.attachments();
        QVERIFY(queue->renderImageEdit(0, 0, 0, 1, 1, 1, false, {})
                    .contains(QStringLiteral("url")));
        QVERIFY(queue->commitImageEdit(0));
        composer.beginReply(QStringLiteral("$reply:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString(), QStringLiteral("$root:example.org"));
        composer.setText(QStringLiteral("edited"));
        composer.send();
        QCOMPARE(client.threadFileSends, 1);
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(client.lastThreadRoot, QStringLiteral("$root:example.org"));
        QCOMPARE(client.lastReplyTo, QStringLiteral("$reply:example.org"));
        // In a thread the typed text never rides as a caption
        // (MessageComposer::takeTextAsCaption): it is its own thread reply.
        QCOMPARE(client.lastCaption, QString());
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
    }

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

    // An unknown server limit (0: not advertised or not yet answered) must not
    // reject anything locally; this file exceeds the old fabricated 100 MiB
    // ceiling.
    void unknownServerLimitDoesNotRejectLocally()
    {
        FakeClient client;
        client.serverLimit = 0;   // unknown
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        QTemporaryDir dir;
        // 101 MiB: over the old fallback ceiling, under no real limit.
        QFile f(dir.filePath(QStringLiteral("huge.bin")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.resize(101ll * 1024 * 1024));
        f.close();

        composer.addAttachment(QUrl::fromLocalFile(f.fileName()));
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    // m.upload.size is the largest accepted payload: exactly at the limit
    // passes, one byte more does not.
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

    // Voice messages are preflighted against the server limit before any
    // upload.
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
        // The refused recording is deleted, not orphaned.
        QVERIFY(!QFile::exists(rec));
    }

    // A voice send failing after the user switched rooms must not surface in
    // the room they are now in.
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
        // Cleanup happens even though reporting was suppressed.
        QVERIFY(!QFile::exists(rec));
    }

    // With the user still in the originating room, the failure is reported:
    // the scoping suppresses only the wrong context.
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

    // B011: the chip's name carries no bidi override.
    void chipNameLosesBidiControls()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QString path = writeFile(
            dir, QStringLiteral("r\u202Etxt.exe\u202C.txt"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::FileNameRole).toString(),
                 QStringLiteral("rtxt.exe.txt"));
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

        // Success: the SDK local echo owns it now; the tray entry leaves.
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});
        QCOMPARE(composer.attachments()->rowCount(), 0);
    }

    // "If I reply to a text with an image it just sends the image": the reply
    // target never reached any attachment send, so the event went out with no
    // m.in_reply_to. Every send path below must carry it.
    void anAttachmentSentWhileReplyingCarriesTheReplyAndSpendsIt()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        composer.beginReply(QStringLiteral("$target:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        QVERIFY(composer.isReplying());

        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
        // Spent exactly as after a text reply.
        QVERIFY(!composer.isReplying());
        QVERIFY(composer.replyingToEventId().isEmpty());
    }

    void anAttachmentSentWithoutAReplyIsPlain()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        client.lastReplyTo = QStringLiteral("stale");
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QVERIFY(client.lastReplyTo.isEmpty());
    }

    // -- Photo metadata ------------------------------------------------------

    void theMetadataModeDefaultsToLocationAndNeverFallsToKeep()
    {
        MessageComposer composer;
        QCOMPARE(composer.imageMetadataMode(), QStringLiteral("location"));
        composer.setImageMetadataMode(QStringLiteral("keep"));
        QCOMPARE(composer.imageMetadataMode(), QStringLiteral("keep"));
        composer.setImageMetadataMode(QStringLiteral("from-the-future"));
        QCOMPARE(composer.imageMetadataMode(), QStringLiteral("location"));
    }

    // The picked file is never modified: a stripped COPY, under the same file
    // name, is what the client is handed.
    void aPickedPhotoIsSentWithoutItsLocationAndTheOriginalIsUntouched()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QByteArray original = jpegWithGps();
        const QString path =
            writeFile(dir, QStringLiteral("holiday.jpg"), original);
        QVERIFY(original.contains(kGpsMarker));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QVERIFY(client.lastFilePath != path);
        QCOMPARE(QFileInfo(client.lastFilePath).fileName(),
                 QStringLiteral("holiday.jpg"));
        const QByteArray sent = readAll(client.lastFilePath);
        QVERIFY(!sent.isEmpty());
        QVERIFY(!sent.contains(kGpsMarker));
        QVERIFY(!imagemeta::containsLocation(sent));
        QImage image;
        QVERIFY(image.loadFromData(sent));
        // The user's own file still has everything.
        QCOMPARE(readAll(path), original);

        // The copy is removed once the SDK has taken the send.
        const QString copy = client.lastFilePath;
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true,
            QString());
        QVERIFY(!QFileInfo::exists(copy));
    }

    void keepSendsTheOriginalFileUntouched()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setImageMetadataMode(QStringLiteral("keep"));
        QTemporaryDir dir;
        const QString path =
            writeFile(dir, QStringLiteral("holiday.jpg"), jpegWithGps());
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastFilePath, path);
    }

    void removeAllAlsoDropsTheRestOfTheExif()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setImageMetadataMode(QStringLiteral("all"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("holiday.jpg"), jpegWithGps())));
        composer.send();
        QCOMPARE(client.fileSends, 1);
        const QByteArray sent = readAll(client.lastFilePath);
        QVERIFY(!sent.contains("Exif"));
    }

    // Paste and clipboard images carry bytes, not a path: same policy.
    void aPastedPhotoIsStrippedToo()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        const QByteArray original = jpegWithGps();
        QVERIFY(composer.attachments()->addImageData(
                    original, QStringLiteral("image/jpeg"), 16, 16)
                    .isEmpty());
        composer.send();
        QCOMPARE(client.byteSends, 1);
        QVERIFY(!client.lastBytes.isEmpty());
        QVERIFY(!client.lastBytes.contains(kGpsMarker));
        QVERIFY(client.lastBytes.size() <= original.size());
    }

    // The user asked for the location to go. If it cannot be removed from a
    // recognised image, the image is not sent.
    void aPhotoWhoseMetadataCannotBeRemovedIsNotSent()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        QByteArray broken = jpegWithGps();
        broken.chop(20);  // no EOI: cannot be vouched for
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("broken.jpg"), broken)));
        composer.send();
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(composer.attachments()->entries().size(), 1);
        QCOMPARE(composer.attachments()->entries().first().state,
                 QStringLiteral("failed"));
        // And "keep" is the explicit way to send it anyway.
        composer.setImageMetadataMode(QStringLiteral("keep"));
        composer.attachments()->retryAt(0);
        composer.send();
        QCOMPARE(client.fileSends, 1);
    }

    void nonImagesAreNotTouched()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        const QString doc =
            writeFile(dir, QStringLiteral("doc.bin"), QByteArray(16, 'a'));
        composer.addAttachment(QUrl::fromLocalFile(doc));
        composer.send();
        QCOMPARE(client.lastFilePath, doc);
    }

    // The typed text rides as the caption (when that setting is on) and the
    // reply survives alongside it: one event, both properties.
    void aCaptionedAttachmentKeepsItsCaptionAndItsReply()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        composer.setSendTextAsCaption(true);
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("photo.png"), tinyPng())));
        composer.beginReply(QStringLiteral("$target:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        composer.setText(QStringLiteral("look at this"));

        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastCaption, QStringLiteral("look at this"));
        QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
    }

    // A video waits for its poster, and the composer's reply chip is gone by
    // the time it dispatches: the target must be captured at send time.
    void aVideoThatWaitsForItsPosterStillCarriesTheReply()
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
        composer.addAttachment(QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("clip.mp4"), tinyMp4Header())));
        QVERIFY(!capturedTag.isEmpty());
        composer.beginReply(QStringLiteral("$target:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());

        composer.send();
        QCOMPARE(client.videoSends, 0);       // held for the poster
        QVERIFY(!composer.isReplying());      // chip already spent

        composer.attachments()->applyPoster(capturedTag, tinyJpeg(),
                                            QSize(320, 180), QSize(1920, 1080),
                                            4200);
        QCOMPARE(client.videoSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$target:example.org"));
    }

    void aClipboardImageAndAVoiceMessageCarryTheReplyToo()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QVERIFY(composer.attachments()->addImageData(
                    tinyPng(), QStringLiteral("image/png"), 1, 1).isEmpty());
        composer.beginReply(QStringLiteral("$bytes:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        composer.send();
        QCOMPARE(client.byteSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$bytes:example.org"));

        QTemporaryDir dir;
        const QString rec =
            writeFile(dir, QStringLiteral("rec.ogg"), QByteArray(32, 'v'));
        composer.beginReply(QStringLiteral("$voice:example.org"),
                            QStringLiteral("Alice"), QStringLiteral("hello"),
                            QString());
        composer.setText(QStringLiteral("draft stays"));
        composer.sendVoiceMessage(rec, QStringLiteral("audio/ogg"), 1200,
                                  QVariantList{ 10, 20, 30 });
        QCOMPARE(client.voiceSends, 1);
        QCOMPARE(client.lastReplyTo, QStringLiteral("$voice:example.org"));
        QVERIFY(!composer.isReplying());
        // A voice message is not the typed text: that stays.
        QCOMPARE(composer.text(), QStringLiteral("draft stays"));
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

    // An attached song is decoded for its length (there is no frame), and
    // that length reaches the send.
    void audioSendCarriesItsDecodedDuration()
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
        const QString path = writeFile(dir, QStringLiteral("song.mp3"),
                                       QByteArray("ID3\x03\x00\x00\x00", 7)
                                           + QByteArray(64, '\x00'));
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QVERIFY2(!capturedTag.isEmpty(),
                 "an audio attachment was never decoded, so nothing could "
                 "have learned its duration");

        // What the decoder reports without a video track: no poster, no frame
        // geometry, a real length.
        composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 185000);
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastDurationMs, 185000);
    }

    // An undecodable file still sends, without a duration.
    void audioThatCannotBeDecodedStillSends()
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
        const QString path = writeFile(dir, QStringLiteral("broken.ogg"),
                                       QByteArray("not really ogg"));
        composer.addAttachment(QUrl::fromLocalFile(path));
        composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 0);
        composer.send();
        QCOMPARE(client.fileSends, 1);
        QCOMPARE(client.lastDurationMs, 0);
    }

    // A video is postered from the picked file before dispatch, and the
    // poster plus the decoder's geometry and duration are what the send
    // declares on the Matrix event.
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

        // Send while the poster is still decoding must not dispatch.
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
        // The decoded frame is the only source of the video's dimensions on
        // the send side.
        QCOMPARE(client.lastVideoWidth, 1920);
        QCOMPARE(client.lastVideoHeight, 1080);
        QCOMPARE(client.lastDurationMs, 4200);
        // 16:9 in, 16:9 out: the poster never distorts the frame.
        QCOMPARE(client.lastThumbWidth * client.lastVideoHeight,
                 client.lastThumbHeight * client.lastVideoWidth);
    }

    // Failed poster extraction does not fail the send: the video goes without
    // a poster.
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

        // The decoder gave up: no poster, geometry or duration.
        composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 0);

        QCOMPARE(client.videoSends, 1);
        QVERIFY(client.lastThumbnail.isEmpty());
        QCOMPARE(client.lastThumbWidth, 0);
        QCOMPARE(client.lastThumbHeight, 0);
        QCOMPARE(client.lastVideoWidth, 0);
        QCOMPARE(client.lastVideoHeight, 0);
        QCOMPARE(client.lastDurationMs, 0);
    }

    // A poster with no pending send dispatches nothing, and a duplicate
    // callback does not send twice.
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

        // The poster resolves before send was pressed.
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

    // A poster for a removed entry (unknown tag) is ignored.
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

    // Non-video attachments request no poster and take the plain send path.
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
        // Rows go one at a time in tray order (B010): the second waits for
        // the first to finish.
        QCOMPARE(client.fileSends, 1);
        Q_EMIT client.attachmentQueueFinished(
            client.lastOpId, QStringLiteral("!room:example.org"), true, {});
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

        // The in-memory path used by clipboard paste.
        const QString reason = composer.attachments()->addImageData(
            tinyPng(), QStringLiteral("image/png"), 1, 1);
        QVERIFY(reason.isEmpty());
        QCOMPARE(composer.attachments()->rowCount(), 1);

        composer.send();
        QCOMPARE(client.byteSends, 1);
        QCOMPARE(client.lastFilename, QStringLiteral("pasted-image.png"));
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
    }

    // ---- SVG: rasterized on send (media/SvgRaster.h) ----

    void init() { lightning::svgraster::hooks() = {}; }
    void cleanup() { lightning::svgraster::hooks() = {}; }

    // The screen refuses every way an SVG can make QtSvg read something
    // outside the document, and passes ordinary self-contained drawings.
    void svgScreenRefusesAnythingThatReachesOutsideTheDocument()
    {
        using lightning::svgraster::screen;
        const QByteArray head =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" "
            "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"10\" "
            "height=\"10\">";
        const QByteArray tail = "</svg>";
        const auto wrap = [&](const QByteArray &body) {
            return head + body + tail;
        };

        const QList<std::pair<QByteArray, QString>> refused = {
            // QtSvg would load these from the local disk.
            { wrap("<image href=\"/home/user/private.png\"/>"),
              QStringLiteral("external_image") },
            { wrap("<image xlink:href=\"data:image/png;base64,AAAA\"/>"),
              QStringLiteral("external_image") },
            { wrap("<s:image xmlns:s=\"http://www.w3.org/2000/svg\" "
                   "href=\"private.png\"/>"),
              QStringLiteral("external_image") },
            { wrap("<filter id=\"f\"><feImage href=\"private.png\"/></filter>"),
              QStringLiteral("external_image") },
            { wrap("<use xlink:href=\"other.svg#a\"/>"),
              QStringLiteral("external_reference") },
            { wrap("<font-face><font-face-src><font-face-uri "
                   "xlink:href=\"font.svg\"/></font-face-src></font-face>"),
              QStringLiteral("external_reference") },
            { wrap("<rect fill=\"url(pattern.png)\"/>"),
              QStringLiteral("external_reference") },
            { wrap("<rect style=\"fill: url( 'file:///x.png' )\"/>"),
              QStringLiteral("external_reference") },
            { wrap("<style>@import 'x.css';</style>"),
              QStringLiteral("external_reference") },
            { wrap("<style><![CDATA[rect { fill: url(x.png) }]]></style>"),
              QStringLiteral("external_reference") },
            // CSS continues past a child element until </style>.
            { wrap("<style>rect {}<x/>@import 'x.css';</style>"),
              QStringLiteral("external_reference") },
            // References that could expand past the budget are refused before
            // any expansion (4000-character entity, 1000 references).
            { QByteArray("<!DOCTYPE svg [<!ENTITY e \"") + QByteArray(4000, 'a')
                  + QByteArray("\">]>")
                  + wrap("<desc t=\"" + QByteArray("&e;").repeated(1000)
                         + "\"/>"),
              QStringLiteral("entities") },
            // Document-level refusals.
            { QByteArray("<?xml version=\"1.0\"?><!DOCTYPE svg [<!ENTITY e "
                         "SYSTEM \"file:///etc/passwd\">]>")
                  + wrap("<text>&e;</text>"),
              QStringLiteral("entities") },
            { QByteArray("<?xml-stylesheet href=\"x.css\"?>") + wrap(""),
              QStringLiteral("processing_instruction") },
            { QByteArray("\x1f\x8b", 2) + QByteArray(30, '\x08'),
              QStringLiteral("compressed") },
            { QByteArray("<html><body/></html>"), QStringLiteral("not_svg") },
            { wrap("<rect>"), QStringLiteral("malformed") },
            { QByteArray(), QStringLiteral("empty") },
            { QByteArray(lightning::svgraster::kMaxSourceBytes + 1, ' '),
              QStringLiteral("too_large") },
            { wrap(QByteArray("<g>").repeated(64) + QByteArray("</g>").repeated(64)),
              QStringLiteral("too_deep") },
            { wrap(QByteArray("<rect/>").repeated(lightning::svgraster::kMaxElements)),
              QStringLiteral("too_many_elements") },
        };
        for (const auto &[bytes, reason] : refused)
            QCOMPARE(screen(bytes), reason);

        const QList<QByteArray> accepted = {
            redSquareSvg(),
            wrap("<defs><linearGradient id=\"g\"/></defs>"
                 "<rect fill=\"url(#g)\"/><use xlink:href=\"#g\"/>"),
            // A hyperlink is inert to QtSvg.
            wrap("<a xlink:href=\"https://example.org/\"><rect/></a>"),
            // Internal entities, as older Illustrator exports declare them.
            QByteArray("<!DOCTYPE svg [<!ENTITY w \"10\">]>")
                + wrap("<rect width=\"&w;\" height=\"&w;\"/>"),
        };
        for (const QByteArray &bytes : accepted)
            QCOMPARE(screen(bytes), QString());
    }

    // A picked SVG becomes a PNG file BEFORE it can send: the client is handed
    // image/png and that file's real dimensions, and nothing named SVG.
    void aPickedSvgIsUploadedAsAPngNeverAsSvg()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();

        QTemporaryDir dir;
        const QString svgPath =
            writeFile(dir, QStringLiteral("logo.svg"), redSquareSvg());
        composer.addAttachment(QUrl::fromLocalFile(svgPath));
        QCOMPARE(model->rowCount(), 1);
        // Until the conversion lands there is nothing to show or send.
        QVERIFY(model->data(model->index(0, 0),
                            AttachmentQueueModel::PreviewSourceRole)
                    .toString().isEmpty());
        composer.send();
        QCOMPARE(client.fileSends, 0);

        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 10000);
        QCOMPARE(client.imageThumbSends, 0);
        QCOMPARE(client.videoSends, 0);
        QCOMPARE(client.byteSends, 0);
        QCOMPARE(client.lastMime, QStringLiteral("image/png"));
        QVERIFY(client.lastFilePath.endsWith(QStringLiteral("/logo.png")));
        QVERIFY(client.lastFilePath != svgPath);

        // The declared geometry is the PNG's real geometry, and its bytes are
        // that PNG: 1600x1200 is above the 1024 floor, so it keeps its size.
        QFile file(client.lastFilePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QCOMPARE(lightning::imagefmt::sniffRasterMime(bytes),
                 QStringLiteral("image/png"));
        const QImage decoded = QImage::fromData(bytes, "PNG");
        QCOMPARE(decoded.size(), QSize(1600, 1200));
        QCOMPARE(client.lastFileWidth, 1600);
        QCOMPARE(client.lastFileHeight, 1200);
        QCOMPARE(decoded.pixelColor(800, 600), QColor(Qt::red));

        // The entry says what it now is.
        QCOMPARE(model->data(model->index(0, 0), AttachmentQueueModel::FileNameRole)
                     .toString(), QStringLiteral("logo.png"));
        QCOMPARE(model->data(model->index(0, 0), AttachmentQueueModel::MimeRole)
                     .toString(), QStringLiteral("image/png"));
        QCOMPARE(model->data(model->index(0, 0), AttachmentQueueModel::SizeBytesRole)
                     .toLongLong(), qint64(bytes.size()));
    }

    // A small icon is converted at a useful size, not at its own tiny one, and
    // the PNG's real size is what the send declares.
    void aTinySvgIsConvertedAtTheAttachmentFloor()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QTemporaryDir dir;
        composer.addAttachment(QUrl::fromLocalFile(writeFile(
            dir, QStringLiteral("icon.svg"),
            QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" "
                              "viewBox=\"0 0 24 12\"><rect width=\"24\" "
                              "height=\"12\"/></svg>"))));
        composer.send();
        QTRY_COMPARE_WITH_TIMEOUT(client.fileSends, 1, 10000);
        QCOMPARE(client.lastFileWidth, 1024);
        QCOMPARE(client.lastFileHeight, 512);
        QCOMPARE(QImage(client.lastFilePath).size(), QSize(1024, 512));
    }

    // A refused SVG fails ITS entry with a sentence naming the reason. It is
    // never uploaded in any form (the old behaviour sent it with no thumbnail).
    void aRefusedSvgFailsWithItsReasonAndSendsNothing()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();

        QTemporaryDir dir;
        const QString secret =
            writeFile(dir, QStringLiteral("private.png"), tinyPng());
        composer.addAttachment(QUrl::fromLocalFile(writeFile(
            dir, QStringLiteral("trap.svg"), svgReadingALocalFile(secret))));
        QCOMPARE(model->rowCount(), 1);
        composer.send();
        QTRY_COMPARE_WITH_TIMEOUT(
            model->data(model->index(0, 0), AttachmentQueueModel::StateRole)
                .toString(),
            QStringLiteral("failed"), 10000);
        const QString error =
            model->data(model->index(0, 0), AttachmentQueueModel::ErrorRole)
                .toString();
        QVERIFY2(error.contains(QStringLiteral("links to other files")),
                 qPrintable(error));
        // B071: the same file would be refused again, so Retry is not offered.
        QVERIFY(!model->data(model->index(0, 0),
                             AttachmentQueueModel::RetryableRole).toBool());
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(client.imageThumbSends, 0);
        QCOMPARE(client.byteSends, 0);
    }

    // An SVG whose PNG would exceed the server's limit fails with the limit
    // message, not a send the server will refuse.
    void aConvertedSvgPastTheUploadLimitFails()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        client.serverLimit = 200;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();
        QTemporaryDir dir;
        // A source under the limit (so the pre-check passes) whose 1024 px PNG
        // is far over it.
        composer.addAttachment(QUrl::fromLocalFile(writeFile(
            dir, QStringLiteral("big.svg"),
            QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" "
                              "viewBox=\"0 0 8 8\"><circle cx=\"4\" cy=\"4\" "
                              "r=\"3\"/></svg>"))));
        QCOMPARE(model->rowCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(
            model->data(model->index(0, 0), AttachmentQueueModel::StateRole)
                .toString(),
            QStringLiteral("failed"), 10000);
        QVERIFY(model->data(model->index(0, 0), AttachmentQueueModel::ErrorRole)
                    .toString().contains(QStringLiteral("upload limit")));
    }

    // The same file twice is refused whether or not it has been converted.
    void theSameSvgCannotBeQueuedTwice()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();
        QTemporaryDir dir;
        const QUrl url = QUrl::fromLocalFile(
            writeFile(dir, QStringLiteral("logo.svg"), redSquareSvg()));
        QVERIFY(model->addFile(url).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!model->entries().at(0).posterPending, 10000);
        QVERIFY(!model->entries().at(0).isSvg); // converted
        QVERIFY(!model->addFile(url).isEmpty());
        QCOMPARE(model->rowCount(), 1);
    }

    // Removing the entry removes the PNG this model wrote, and only that: the
    // user's own SVG is never touched.
    void removingAnEntryDeletesItsConvertedPngButNeverTheSource()
    {
        if (!lightning::svgraster::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        lightning::svgraster::hooks().inProcess = true;
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        AttachmentQueueModel *model = composer.attachments();
        QTemporaryDir dir;
        const QString svgPath =
            writeFile(dir, QStringLiteral("logo.svg"), redSquareSvg());
        QVERIFY(model->addFile(QUrl::fromLocalFile(svgPath)).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!model->entries().at(0).posterPending, 10000);
        const QString png = model->entries().at(0).localPath;
        QVERIFY(QFileInfo::exists(png));
        QVERIFY(png != svgPath);
        model->removeAt(0);
        QVERIFY(!QFileInfo::exists(png));
        QVERIFY(QFileInfo::exists(svgPath));
    }

    // Received SVG is untouched: the shape sniff on the bytes a recipient would
    // fetch still refuses SVG under every label.
    void receivedSvgStaysRefused()
    {
        const QByteArray svg = redSquareSvg();
        QVERIFY(lightning::imagefmt::sniffRasterMime(svg).isEmpty());
        QVERIFY(lightning::imagefmt::sniffRasterMime(
                    QByteArray("<?xml version=\"1.0\"?>") + svg).isEmpty());
        // The converted output, by contrast, is a raster the sniff accepts.
        if (lightning::svgraster::available()) {
            const auto result = lightning::svgraster::rasterize(
                svg, lightning::svgraster::attachmentPolicy());
            QCOMPARE(lightning::imagefmt::sniffRasterMime(result.png),
                     QStringLiteral("image/png"));
        }
    }

    // --- #41: a picked video or audio file is classified by what it is ---

    // The shared MIME database names Matroska and ASF generically by content
    // (application/x-matroska, application/vnd.ms-asf), so before the fix an
    // .mkv (OBS's default recording), .mka, .wmv or .wma was queued as a plain
    // file and went out as m.file. The extension may NARROW a proven container
    // to the audio or video type that inherits from it, and nothing else.
    void detectMimeNarrowsAGenericContainerByItsExtension()
    {
        QTemporaryDir dir;
        struct Case {
            const char *name;
            QByteArray bytes;
            const char *expected;
        };
        const Case cases[] = {
            {"recording.mkv", tinyMatroskaHeader(), "video/x-matroska"},
            {"RECORDING.MKV", tinyMatroskaHeader(), "video/x-matroska"},
            {"album.mka", tinyMatroskaHeader(), "audio/x-matroska"},
            {"clip.wmv", tinyAsfHeader(), "video/x-ms-wmv"},
            {"song.wma", tinyAsfHeader(), "audio/x-ms-wma"},
            // Content still wins over a name that disagrees with it.
            {"picture.mkv", tinyPng(), "image/png"},
            {"clip.mp4", tinyMp4Header(), "video/mp4"},
            // Proved nothing: the extension decides, as before.
            {"camera.mp4", unknownBrandMp4Header(), "video/mp4"},
            // A container with no name to narrow it stays a file.
            {"no-extension", tinyMatroskaHeader(), "application/x-matroska"},
        };
        for (const Case &c : cases) {
            const QString path =
                writeFile(dir, QString::fromLatin1(c.name), c.bytes);
            QVERIFY(!path.isEmpty());
            QCOMPARE(AttachmentQueueModel::detectMime(path),
                     QString::fromLatin1(c.expected));
        }
        // Only a proven MEDIA container is narrowed. A playlist name is an
        // audio/* type inheriting text/plain, and .yt a video/* type inheriting
        // application/zip: neither is sound or picture.
        const QString playlist = writeFile(
            dir, QStringLiteral("list.vlc"),
            QByteArrayLiteral("song one.mp3\nsong two.mp3\n"));
        QVERIFY(!AttachmentQueueModel::detectMime(playlist)
                     .startsWith(QLatin1String("audio/")));
        const QString archive = writeFile(
            dir, QStringLiteral("bundle.yt"),
            QByteArrayLiteral("PK\x03\x04") + QByteArray(64, '\0'));
        QCOMPARE(AttachmentQueueModel::detectMime(archive),
                 QStringLiteral("application/zip"));
    }

    // End to end through the composer: an .mkv takes the VIDEO send path, with
    // its poster, geometry and duration; before the fix it took the plain
    // attachment path as application/x-matroska.
    void aPickedMatroskaVideoIsSentAsAVideo()
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
        const QString path = writeFile(dir, QStringLiteral("obs-recording.mkv"),
                                       tinyMatroskaHeader());
        composer.addAttachment(QUrl::fromLocalFile(path));
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QVERIFY2(!capturedTag.isEmpty(),
                 "a Matroska video was never postered: it was not classified "
                 "as a video");
        composer.send();
        const QByteArray poster = tinyJpeg();
        composer.attachments()->applyPoster(capturedTag, poster, QSize(320, 180),
                                            QSize(1920, 1080), 9000);
        QCOMPARE(client.videoSends, 1);
        QCOMPARE(client.fileSends, 0);
        QCOMPARE(client.lastMime, QStringLiteral("video/x-matroska"));
        QCOMPARE(client.lastThumbnail, poster);
        QCOMPARE(client.lastDurationMs, 9000);
    }

    // The audio twin: an .mka or .wma is sent with an audio/* type (the Rust
    // side turns that into m.audio) and its decoded duration.
    void aPickedMatroskaOrWmaAudioIsSentAsAudio()
    {
        for (const auto &[name, bytes, mime] :
             {std::tuple{"album.mka", tinyMatroskaHeader(), "audio/x-matroska"},
              std::tuple{"song.wma", tinyAsfHeader(), "audio/x-ms-wma"}}) {
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
                writeFile(dir, QString::fromLatin1(name), bytes);
            composer.addAttachment(QUrl::fromLocalFile(path));
            QVERIFY2(!capturedTag.isEmpty(),
                     "audio was never decoded for its duration: it was not "
                     "classified as audio");
            composer.attachments()->applyPoster(capturedTag, {}, {}, {}, 61000);
            composer.send();
            QCOMPARE(client.fileSends, 1);
            QCOMPARE(client.videoSends, 0);
            QCOMPARE(client.lastMime, QString::fromLatin1(mime));
            QCOMPARE(client.lastDurationMs, 61000);
        }
    }

    // --- #42: a dropped file reaches the queue ------------------------------

    // "file://localhost/x" is the local path /x, but QUrl makes it the
    // UNC-style "//localhost/x", which never exists: such a drop failed with
    // "That file cannot be read." before the fix.
    void aLocalhostFileUrlIsTheLocalPath()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir, QStringLiteral("a b#1.bin"),
                                       QByteArray(16, 'a'));
        QUrl localhost = QUrl::fromLocalFile(path);
        localhost.setHost(QStringLiteral("LocalHost"));
        QVERIFY(!QFileInfo(localhost.toLocalFile()).isReadable());
        QCOMPARE(AttachmentQueueModel::normalizeDroppedUrl(localhost)
                     .toLocalFile(),
                 path);
        // A sloppy uri-list wrote the '#' unescaped: QUrl reads a fragment.
        const QUrl sloppy = QUrl::fromEncoded(
            QUrl::fromLocalFile(dir.path()).toEncoded()
            + QByteArrayLiteral("/a%20b#1.bin"));
        QCOMPARE(sloppy.fragment(), QStringLiteral("1.bin"));
        QCOMPARE(AttachmentQueueModel::normalizeDroppedUrl(sloppy)
                     .toLocalFile(),
                 path);
        // Anything that is not a local file is left alone.
        const QUrl web(QStringLiteral("https://example.org/a#b"));
        QCOMPARE(AttachmentQueueModel::normalizeDroppedUrl(web), web);

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        composer.addDroppedAttachments({localhost}, QString());
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    // GTK writes the portal key with its NUL terminator, KDE without.
    void thePortalKeyIsReadWithOrWithoutItsNul()
    {
        QCOMPARE(AttachmentQueueModel::portalKeyFrom(
                     QByteArray("12345\0", 6)),
                 QStringLiteral("12345"));
        QCOMPARE(AttachmentQueueModel::portalKeyFrom("12345"),
                 QStringLiteral("12345"));
        QCOMPARE(AttachmentQueueModel::portalKeyFrom({}), QString());
        QMimeData mime;
        QCOMPARE(AttachmentQueueModel::portalKeyFromMimeData(&mime), QString());
        mime.setData(QStringLiteral("application/vnd.portal.files"),
                     QByteArray("legacy\0", 7));
        QCOMPARE(AttachmentQueueModel::portalKeyFromMimeData(&mime),
                 QStringLiteral("legacy"));
        mime.setData(QStringLiteral("application/vnd.portal.filetransfer"),
                     "current");
        QCOMPARE(AttachmentQueueModel::portalKeyFromMimeData(&mime),
                 QStringLiteral("current"));
    }

    // A Flatpak build is handed the HOST path, which does not exist inside the
    // sandbox; the file is reachable through the FileTransfer portal key the
    // file manager put beside it. Before the fix the key was never read and the
    // drop failed with "That file cannot be read."
    void aSandboxedDropIsResolvedThroughThePortal()
    {
        const auto reset = qScopeGuard([] {
            AttachmentQueueModel::setPortalRetrieveHook({});
        });
        QTemporaryDir dir;
        const QString exported = writeFile(dir, QStringLiteral("clip.bin"),
                                           QByteArray(32, 'v'));
        const QUrl hostPath = QUrl::fromLocalFile(
            dir.filePath(QStringLiteral("not-in-the-sandbox/clip.bin")));
        QStringList askedKeys;
        AttachmentQueueModel::setPortalRetrieveHook(
            [&](const QString &key,
                std::function<void(bool, const QStringList &)> done) {
                askedKeys.append(key);
                done(true, {exported});
            });

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);
        composer.addDroppedAttachments({hostPath}, QStringLiteral("key-1"));
        QCOMPARE(askedKeys, QStringList{QStringLiteral("key-1")});
        QCOMPARE(rejected.count(), 0);
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(composer.attachments()
                     ->data(composer.attachments()->index(0, 0),
                            AttachmentQueueModel::LocalUrlRole)
                     .toUrl()
                     .toLocalFile(),
                 exported);

        // A readable drop never asks the portal, key or not.
        composer.attachments()->clearAll();
        const QString readable = writeFile(dir, QStringLiteral("here.bin"),
                                           QByteArray(8, 'h'));
        composer.addDroppedAttachments({QUrl::fromLocalFile(readable)},
                                       QStringLiteral("key-2"));
        QCOMPARE(askedKeys.size(), 1);
        QCOMPARE(composer.attachments()->rowCount(), 1);
    }

    // The portal failing falls back to the dropped URLs, so the user still
    // gets addFile's refusal, and in a sandbox it names the way that works.
    // An answer that arrives after a room switch queues nothing.
    void aPortalFailureOrALateAnswerIsHandled()
    {
        const auto reset = qScopeGuard([] {
            AttachmentQueueModel::setPortalRetrieveHook({});
            AttachmentQueueModel::setSandboxKindForTesting({});
        });
        AttachmentQueueModel::setSandboxKindForTesting(QStringLiteral("flatpak"));
        std::function<void(bool, const QStringList &)> pending;
        AttachmentQueueModel::setPortalRetrieveHook(
            [&](const QString &,
                std::function<void(bool, const QStringList &)> done) {
                pending = std::move(done);
            });
        QTemporaryDir dir;
        const QUrl hostPath = QUrl::fromLocalFile(
            dir.filePath(QStringLiteral("elsewhere/clip.bin")));

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));
        QSignalSpy rejected(&composer, &MessageComposer::attachmentRejected);

        composer.addDroppedAttachments({hostPath}, QStringLiteral("k"));
        QVERIFY(pending);
        pending(false, {});
        QCOMPARE(rejected.count(), 1);
        QVERIFY2(rejected.at(0).at(0).toString().contains(
                     QLatin1String("sandbox")),
                 qPrintable(rejected.at(0).at(0).toString()));
        QCOMPARE(composer.attachments()->rowCount(), 0);

        // Without a key in a sandbox: the same sentence, no portal call.
        pending = nullptr;
        composer.addDroppedAttachments({hostPath}, QString());
        QVERIFY(!pending);
        QCOMPARE(rejected.count(), 2);

        // Room switch while the portal is answering.
        const QString exported = writeFile(dir, QStringLiteral("late.bin"),
                                           QByteArray(8, 'l'));
        composer.addDroppedAttachments({hostPath}, QStringLiteral("k2"));
        QVERIFY(pending);
        composer.setRoomId(QStringLiteral("!other:example.org"));
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 0);
        QCOMPARE(rejected.count(), 2);
    }

    // A portal answer for a drop made in one account must never stage a file in
    // the next one. The room id alone cannot tell: room A, sign-out, the next
    // account opens room A (§9).
    void aLatePortalAnswerNeverCrossesIntoAnotherAccount()
    {
        const auto reset = qScopeGuard([] {
            AttachmentQueueModel::setPortalRetrieveHook({});
        });
        std::function<void(bool, const QStringList &)> pending;
        AttachmentQueueModel::setPortalRetrieveHook(
            [&](const QString &,
                std::function<void(bool, const QStringList &)> done) {
                pending = std::move(done);
            });
        QTemporaryDir dir;
        const QString exported = writeFile(dir, QStringLiteral("late.bin"),
                                           QByteArray(8, 'l'));
        const QUrl hostPath = QUrl::fromLocalFile(
            dir.filePath(QStringLiteral("elsewhere/clip.bin")));
        const QString room = QStringLiteral("!room:example.org");

        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(room);

        // Another account, same room id, before the answer.
        composer.addDroppedAttachments({hostPath}, QStringLiteral("k1"));
        QVERIFY(pending);
        client.userId = QStringLiteral("@someone-else:example.org");
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 0);

        // Sign-out and back into the same room before the answer.
        pending = nullptr;
        composer.addDroppedAttachments({hostPath}, QStringLiteral("k2"));
        QVERIFY(pending);
        client.logout();
        composer.setRoomId(room);
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 0);

        // Control: the same answer in the same session is queued and the bar
        // is told, so it can focus the editor after an async drop.
        QSignalSpy resolved(&composer,
                            &MessageComposer::droppedAttachmentsResolved);
        pending = nullptr;
        composer.addDroppedAttachments({hostPath}, QStringLiteral("k3"));
        QVERIFY(pending);
        pending(true, {exported});
        QCOMPARE(composer.attachments()->rowCount(), 1);
        QCOMPARE(resolved.count(), 1);
    }
};

QTEST_GUILESS_MAIN(AttachmentQueueTest)
#include "AttachmentQueueTest.moc"
