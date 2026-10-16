// Several attachments sent together go out one at a time, in tray order: the
// SDK send queue fixes its order when a file has been read and cached, so
// dispatching every row at once lets a small file overtake a large one.

#include "matrix/MatrixClient.h"
#include "models/MessageComposer.h"

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

} // namespace

class AttachmentOrderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void rowsAreDispatchedOneAtATimeInTrayOrder()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        const QString room = QStringLiteral("!room:example.org");
        composer.setRoomId(room);

        QTemporaryDir dir;
        for (const char *name : { "a.bin", "b.bin", "c.bin" })
            composer.addAttachment(QUrl::fromLocalFile(
                writeFile(dir, QString::fromLatin1(name), QByteArray(16, 'a'))));
        QCOMPARE(composer.attachments()->rowCount(), 3);

        composer.send();
        // Only the first is in flight; the others wait for its result.
        QCOMPARE(client.fileSends, 1);
        QVERIFY(client.lastFilePath.endsWith(QStringLiteral("a.bin")));

        Q_EMIT client.attachmentQueueFinished(client.lastOpId, room, true, {});
        QCOMPARE(client.fileSends, 2);
        QVERIFY(client.lastFilePath.endsWith(QStringLiteral("b.bin")));

        // A failed row does not strand the ones behind it.
        Q_EMIT client.attachmentQueueFinished(client.lastOpId, room, false,
                                              QStringLiteral("rejected"));
        QCOMPARE(client.fileSends, 3);
        QVERIFY(client.lastFilePath.endsWith(QStringLiteral("c.bin")));
    }

    void leavingTheRoomStillSendsWhatWasAlreadySent()
    {
        FakeClient client;
        MessageComposer composer;
        composer.setClient(&client);
        composer.setRoomId(QStringLiteral("!room:example.org"));

        QTemporaryDir dir;
        for (const char *name : { "a.bin", "b.bin" })
            composer.addAttachment(QUrl::fromLocalFile(
                writeFile(dir, QString::fromLatin1(name), QByteArray(16, 'a'))));
        composer.send();
        QCOMPARE(client.fileSends, 1);

        // The tray belongs to the room, but a row the user already sent must
        // not be dropped with it.
        composer.setRoomId(QStringLiteral("!other:example.org"));
        QCOMPARE(client.fileSends, 2);
    }
};

QTEST_GUILESS_MAIN(AttachmentOrderTest)
#include "AttachmentOrderTest.moc"
