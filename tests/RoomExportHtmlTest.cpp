// The HTML room export and the controller that runs an export.
//
// Writer (pure, into a buffer):
//   * the page carries the strict CSP first in <head> and no <script> at all;
//   * a plain body is escaped, a formatted body goes through the timeline's
//     sanitizer and loses remote images, event handlers and non-http links;
//   * an image is embedded as a data: URI only when its bytes are a raster
//     format, SVG never (by type, by name or by content);
//   * the size-limit, failure and not-included placeholders say what happened;
//   * an encrypted room without the opt-in withholds bodies AND attachments;
//   * the chunked writer produces exactly what the one-shot writer does.
//
// Controller (against a scripted client):
//   * walk rows become events, reversed into time order, with the thread,
//     deleted and undecryptable markers and the opaque media handle;
//   * a full HTML export fetches attachments through fetchMedia by handle,
//     honours the budget, commits the file and releases the handles;
//   * cancel stops the walk, leaves no file and ignores a late page;
//   * an encrypted room without the opt-in never fetches an attachment.

#include "matrix/MatrixClient.h"
#include "models/RoomExport.h"
#include "models/RoomExportController.h"
#include "models/RoomExportHtml.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

namespace {

TimelineEvent message(const QString &id, const QString &body, qint64 tsMs,
                      const QString &sender = QStringLiteral("@a:example.org"),
                      const QString &display = QStringLiteral("Ann"))
{
    TimelineEvent e;
    e.eventId = id;
    e.roomId = QStringLiteral("!r:example.org");
    e.sender = sender;
    e.senderDisplayName = display;
    e.body = body;
    e.type = TimelineEvent::TextMessage;
    e.timestamp = QDateTime::fromMSecsSinceEpoch(tsMs, QTimeZone::UTC);
    return e;
}

TimelineEvent image(const QString &id, const QString &name,
                    const QString &mime, qint64 tsMs)
{
    TimelineEvent e = message(id, name, tsMs);
    e.type = TimelineEvent::Image;
    e.mediaFilename = name;
    e.mediaMimetype = mime;
    e.mediaKey = QStringLiteral("export:1:") + id;
    return e;
}

roomexport::Options plainRoom()
{
    roomexport::Options o;
    o.roomId = QStringLiteral("!r:example.org");
    o.roomName = QStringLiteral("The Lounge");
    o.exportedBy = QStringLiteral("@me:example.org");
    o.use24HourClock = true;
    o.includeAttachments = true;
    o.attachmentBudgetBytes = 10 * 1024 * 1024;
    return o;
}

// The smallest thing every sniffer accepts as a PNG: the signature plus a
// little more.
QByteArray pngBytes()
{
    QByteArray b("\x89PNG\r\n\x1a\n", 8);
    b.append(QByteArray("\x00\x00\x00\x0dIHDR-pixel-data-0123456789", 30));
    return b;
}

QByteArray svgBytes()
{
    return QByteArrayLiteral("<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3"
                             ".org/2000/svg\" onload=\"alert(1)\"></svg>");
}

QString html(const QList<TimelineEvent> &events, const roomexport::Options &o,
             const QHash<QString, roomexport::Attachment> &a = {})
{
    return QString::fromUtf8(roomexport::renderHtml(events, o, a));
}

roomexport::Attachment embedded(const QByteArray &bytes)
{
    roomexport::Attachment a;
    a.state = roomexport::AttachmentState::Embedded;
    a.bytes = bytes;
    return a;
}

// A client that records what the controller asks for and answers on command.
class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    bool walkSupported = true;
    quint64 nextOp = 500;
    quint64 walkOp = 0;
    int walkMode = -99;
    qint64 walkLimit = 0;
    QList<quint64> cancelledExports;
    QStringList fetchedKeys;
    QList<quint64> fetchOps;
    QList<quint64> cancelledFetches;

    bool supportsRoomExportWalk() const override { return walkSupported; }
    quint64 startRoomExport(const QString &, int mode, qint64 limit, qint64,
                            qint64) override
    {
        walkMode = mode;
        walkLimit = limit;
        walkOp = nextOp++;
        return walkOp;
    }
    void cancelRoomExport(quint64 opId) override
    {
        cancelledExports.append(opId);
    }
    quint64 fetchMedia(const QString &mediaKey, int, int) override
    {
        fetchedKeys.append(mediaKey);
        const quint64 op = nextOp++;
        fetchOps.append(op);
        return op;
    }
    void cancelMediaFetch(quint64 opId) override
    {
        cancelledFetches.append(opId);
    }

    void page(quint64 op, const QVariantList &rows, bool done,
              const QString &stop = QStringLiteral("start"))
    {
        Q_EMIT roomExportPage(op, QStringLiteral("!r:example.org"), rows,
                              rows.size(), rows.size(), 0, done,
                              done ? stop : QString(), false);
    }
    void ready(quint64 op, const QByteArray &bytes)
    {
        Q_EMIT mediaReady(op, QString(), 0, bytes, QString(), QString());
    }

    // Pure virtuals (inert).
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

// Every entry in `dir`, hidden ones included: QSaveFile writes to a sibling
// temporary file, and a cancelled export must leave that gone too, not only
// the target absent.
QStringList leftovers(const QTemporaryDir &dir)
{
    return QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden
                                      | QDir::System | QDir::NoDotAndDotDot);
}

QVariantMap walkRow(const QString &id, const QString &kind, qint64 ts,
                    const QString &body = QString())
{
    return QVariantMap{
        { QStringLiteral("eventId"), id },
        { QStringLiteral("sender"), QStringLiteral("@a:example.org") },
        { QStringLiteral("senderName"), QStringLiteral("Ann") },
        { QStringLiteral("timestampMs"), ts },
        { QStringLiteral("kind"), kind },
        { QStringLiteral("body"), body },
    };
}

QVariantMap walkImageRow(const QString &id, qint64 ts, qint64 size)
{
    QVariantMap row = walkRow(id, QStringLiteral("image"), ts,
                              QStringLiteral("photo.png"));
    row.insert(QStringLiteral("filename"), QStringLiteral("photo.png"));
    row.insert(QStringLiteral("mimetype"), QStringLiteral("image/png"));
    row.insert(QStringLiteral("size"), size);
    row.insert(QStringLiteral("mediaKey"),
               QStringLiteral("export:500:") + id);
    return row;
}

RoomExportController::Job htmlJob(const QString &path)
{
    RoomExportController::Job job;
    job.roomId = QStringLiteral("!r:example.org");
    job.filePath = path;
    job.format = roomexport::Format::Html;
    job.options = plainRoom();
    job.range = RoomExportController::Range::Latest;
    job.limit = 10;
    return job;
}

} // namespace

class RoomExportHtmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ── The page cannot reach out or run anything ────────────────────────
    void thePageCarriesTheStrictPolicyFirstAndNoScript()
    {
        const QString page = html(
            { message(QStringLiteral("$1"), QStringLiteral("hi"), 1000) },
            plainRoom());
        const qsizetype head = page.indexOf(QStringLiteral("<head>"));
        QVERIFY(head >= 0);
        // The first element after <head> is the policy.
        const QString afterHead =
            page.mid(head + 6).trimmed().left(80);
        QVERIFY2(afterHead.startsWith(QStringLiteral(
                     "<meta http-equiv=\"Content-Security-Policy\"")),
                 qPrintable(afterHead));
        QVERIFY(page.contains(QStringLiteral(
            "default-src 'none'; img-src data:; media-src data:; "
            "style-src 'unsafe-inline'")));
        QVERIFY(roomexport::contentSecurityPolicy().startsWith(
            QStringLiteral("default-src 'none'")));
        QVERIFY2(!page.contains(QStringLiteral("<script"), Qt::CaseInsensitive),
                 "the export page contains a script element");
        QVERIFY(page.contains(QStringLiteral("Exported from Lightning")));
        QVERIFY(page.contains(QStringLiteral("Messages in this file: 1")));
    }

    void aPlainBodyIsEscaped()
    {
        const QString page = html(
            { message(QStringLiteral("$1"),
                      QStringLiteral("<script>alert(1)</script> & <b>x</b>"),
                      1000) },
            plainRoom());
        QVERIFY(!page.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(page.contains(
            QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt;")));
        QVERIFY(page.contains(QStringLiteral("&lt;b&gt;x&lt;/b&gt;")));
        QVERIFY(page.contains(QStringLiteral("&amp;")));
    }

    void aPlainBodysHttpUrlBecomesALinkAndNothingElseDoes()
    {
        const QString page = html(
            { message(QStringLiteral("$1"),
                      QStringLiteral("see https://example.org/a?b=1&c=2. "
                                     "and javascript:alert(1)"),
                      1000) },
            plainRoom());
        QVERIFY(page.contains(QStringLiteral(
            "<a href=\"https://example.org/a?b=1&amp;c=2\"")));
        QVERIFY(!page.contains(QStringLiteral("href=\"javascript:")));
    }

    void aFormattedBodyIsSanitizedAndLosesRemoteImagesAndHandlers()
    {
        TimelineEvent e = message(QStringLiteral("$1"), QStringLiteral("x"),
                                  1000);
        e.formattedBody = QStringLiteral(
            "<img src=\"https://evil.example/beacon.png\" "
            "onerror=\"alert(1)\"><b>bold</b> "
            "<a href=\"javascript:alert(2)\">click</a> "
            "<a href=\"https://example.org/\">ok</a>"
            "<script>alert(3)</script>"
            "<a href=\"https://matrix.to/#/@bob:example.org\">Bob</a>");
        const QString page = html({ e }, plainRoom());
        QVERIFY2(!page.contains(QStringLiteral("evil.example")),
                 "a remote image source survived");
        QVERIFY(!page.contains(QStringLiteral("onerror"), Qt::CaseInsensitive));
        QVERIFY(!page.contains(QStringLiteral("javascript:"),
                               Qt::CaseInsensitive));
        QVERIFY(!page.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!page.contains(QStringLiteral("alert(3)")));
        QVERIFY(page.contains(QStringLiteral("<b>bold</b>")));
        QVERIFY(page.contains(QStringLiteral("<a href=\"https://example.org/\"")));
        // The mention is text in a file: no internal mention: scheme.
        QVERIFY(!page.contains(QStringLiteral("mention:")));
        // No <img> of any kind outside an embedded data: URI.
        QVERIFY(!page.contains(QStringLiteral("<img")));
    }

    // ── Attachments ──────────────────────────────────────────────────────
    void anImageIsEmbeddedAsADataUriFromItsBytes()
    {
        const TimelineEvent img = image(QStringLiteral("$img"),
                                        QStringLiteral("photo.png"),
                                        // A lying type: the bytes decide.
                                        QStringLiteral("application/x-evil"),
                                        1000);
        const QByteArray bytes = pngBytes();
        const QString page = html({ img }, plainRoom(),
                                  { { img.eventId, embedded(bytes) } });
        const QString expected = QStringLiteral("src=\"data:image/png;base64,")
            + QString::fromLatin1(bytes.toBase64()) + QStringLiteral("\"");
        QVERIFY2(page.contains(expected), qPrintable(page.right(600)));
        // Never the handle, never an mxc.
        QVERIFY(!page.contains(QStringLiteral("export:1:")));
        QVERIFY(!page.contains(QStringLiteral("mxc://")));
    }

    void svgIsNeverEmbeddedByTypeNameOrContent()
    {
        // Declared SVG.
        const TimelineEvent declared = image(QStringLiteral("$s1"),
                                             QStringLiteral("logo.svg"),
                                             QStringLiteral("image/svg+xml"),
                                             1000);
        // Declared PNG, bytes are SVG.
        const TimelineEvent disguised = image(QStringLiteral("$s2"),
                                              QStringLiteral("cat.png"),
                                              QStringLiteral("image/png"),
                                              2000);
        const QByteArray svg = svgBytes();
        const QString page = html(
            { declared, disguised }, plainRoom(),
            { { declared.eventId, embedded(svg) },
              { disguised.eventId, embedded(svg) } });
        QVERIFY(!page.contains(QStringLiteral("image/svg"), Qt::CaseInsensitive));
        QVERIFY(!page.contains(QString::fromLatin1(svg.toBase64())));
        QVERIFY(!page.contains(QStringLiteral("onload")));
        QCOMPARE(page.count(QStringLiteral("SVG images are never embedded")), 2);
        QVERIFY(roomexport::declaredSvg(declared));
        QVERIFY(!roomexport::declaredSvg(disguised));
        QVERIFY(roomexport::looksLikeMarkupOrCompressed(svg));
    }

    void anUnrecognisedImageIsAnInertDownloadNotAnImage()
    {
        const TimelineEvent img = image(QStringLiteral("$x"),
                                        QStringLiteral("weird.png"),
                                        QStringLiteral("image/png"), 1000);
        const QByteArray bytes = QByteArrayLiteral("not an image at all");
        const QString page = html({ img }, plainRoom(),
                                  { { img.eventId, embedded(bytes) } });
        QVERIFY(page.contains(QStringLiteral(
            "href=\"data:application/octet-stream;base64,")));
        QVERIFY(page.contains(QStringLiteral("download=\"weird.png\"")));
        QVERIFY(!page.contains(QStringLiteral("<img")));
    }

    void theSizeLimitAndFailurePlaceholdersSayWhatHappened()
    {
        const TimelineEvent big = image(QStringLiteral("$big"),
                                        QStringLiteral("huge.png"),
                                        QStringLiteral("image/png"), 1000);
        const TimelineEvent lost = image(QStringLiteral("$lost"),
                                         QStringLiteral("gone.png"),
                                         QStringLiteral("image/png"), 2000);
        roomexport::Attachment over;
        over.state = roomexport::AttachmentState::OverLimit;
        roomexport::Attachment failed;
        failed.state = roomexport::AttachmentState::Failed;
        const QString page = html({ big, lost }, plainRoom(),
                                  { { big.eventId, over },
                                    { lost.eventId, failed } });
        QVERIFY(page.contains(QStringLiteral(
            "huge.png — not included: over the size limit")));
        QVERIFY(page.contains(QStringLiteral(
            "gone.png — not included: it could not be downloaded")));
        QVERIFY(!page.contains(QStringLiteral("data:")
                               + QStringLiteral("image")));
    }

    void anEncryptedRoomWithoutTheOptInWithholdsBodiesAndAttachments()
    {
        TimelineEvent text = message(QStringLiteral("$1"),
                                     QStringLiteral("the secret plan"), 1000);
        text.formattedBody = QStringLiteral("<b>the secret plan</b>");
        TimelineEvent img = image(QStringLiteral("$img"),
                                  QStringLiteral("secret-photo.png"),
                                  QStringLiteral("image/png"), 2000);
        roomexport::Options o = plainRoom();
        o.encrypted = true;
        o.allowEncryptedPlaintext = false;
        const QByteArray bytes = pngBytes();
        const QString page = html({ text, img }, o,
                                  { { img.eventId, embedded(bytes) } });
        QVERIFY(!page.contains(QStringLiteral("the secret plan")));
        QVERIFY(!page.contains(QStringLiteral("secret-photo")));
        QVERIFY(!page.contains(QStringLiteral("base64,")));
        QVERIFY(!page.contains(QString::fromLatin1(bytes.toBase64())));
        // The shape survives: who and when.
        QVERIFY(page.contains(QStringLiteral("Ann")));
        QCOMPARE(page.count(QStringLiteral("message withheld")), 2);
        QVERIFY(page.contains(QStringLiteral("ENCRYPTED")));

        // With the opt-in, both are written.
        o.allowEncryptedPlaintext = true;
        const QString allowed = html({ text, img }, o,
                                     { { img.eventId, embedded(bytes) } });
        QVERIFY(allowed.contains(QStringLiteral("the secret plan")));
        QVERIFY(allowed.contains(QString::fromLatin1(bytes.toBase64())));
    }

    void deletedUndecryptableAndThreadRowsAreMarked()
    {
        TimelineEvent deleted = message(QStringLiteral("$d"), QString(), 1000);
        deleted.redacted = true;
        TimelineEvent utd = message(QStringLiteral("$u"), QString(), 2000);
        utd.type = TimelineEvent::Unknown;
        utd.undecryptable = true;
        TimelineEvent threaded = message(QStringLiteral("$t"),
                                         QStringLiteral("in a thread"), 3000);
        threaded.threadRootId = QStringLiteral("$root");
        threaded.replyToEventId = QStringLiteral("$d");
        roomexport::Options o = plainRoom();
        o.undecryptableCount = 1;
        const QString page = html({ deleted, utd, threaded }, o);
        QVERIFY(page.contains(QStringLiteral("[message deleted]")));
        QVERIFY(page.contains(QStringLiteral("unable to decrypt")));
        QVERIFY(page.contains(QStringLiteral("in thread")));
        QVERIFY(page.contains(QStringLiteral("href=\"#e-%24d\"")));
        QVERIFY(page.contains(QStringLiteral(
            "Messages that could not be decrypted: 1")));

        // The text format says the same things.
        const QString text = roomexport::renderPlainText(
            { deleted, utd, threaded }, o);
        QVERIFY(text.contains(QStringLiteral("[message deleted]")));
        QVERIFY(text.contains(QStringLiteral("[in thread]")));
    }

    void theChunkedWriterMatchesTheOneShotWriter()
    {
        const TimelineEvent img = image(QStringLiteral("$img"),
                                        QStringLiteral("photo.png"),
                                        QStringLiteral("image/png"), 1000);
        QByteArray bytes = pngBytes();
        // Not a multiple of the chunk or of 3.
        bytes.append(QByteArray(10007, 'z'));
        const roomexport::Attachment a = embedded(bytes);

        QByteArray oneShot;
        {
            QBuffer buffer(&oneShot);
            buffer.open(QIODevice::WriteOnly);
            roomexport::HtmlWriter w(&buffer, plainRoom());
            QVERIFY(w.writeEvent(img, a));
        }
        QByteArray chunked;
        {
            QBuffer buffer(&chunked);
            buffer.open(QIODevice::WriteOnly);
            roomexport::HtmlWriter w(&buffer, plainRoom());
            QVERIFY(w.beginEvent(img, a) > 0);
            int turns = 0;
            while (w.writePayloadChunk(1000))
                ++turns;
            QVERIFY(turns > 5);
            QVERIFY(w.endEvent());
        }
        QCOMPARE(chunked, oneShot);
    }

    void aDownloadNameIsALeaf()
    {
        QCOMPARE(roomexport::safeDownloadName(QStringLiteral("../../x.sh")),
                 QStringLiteral("-..-x.sh"));
        QVERIFY(!roomexport::safeDownloadName(QStringLiteral("a/b\\c:d"))
                     .contains(QLatin1Char('/')));
        QCOMPARE(roomexport::safeDownloadName(QStringLiteral("...")),
                 QStringLiteral("attachment"));
    }

    void theSuggestedNameForHtmlEndsInHtml()
    {
        QVERIFY(roomexport::suggestedFileName(plainRoom(),
                                              roomexport::Format::Html)
                    .endsWith(QStringLiteral(".html")));
    }

    // ── Controller ───────────────────────────────────────────────────────
    void aWalkRowBecomesAnEventWithItsMarkers()
    {
        QVariantMap row = walkImageRow(QStringLiteral("$i"), 5000, 42);
        row.insert(QStringLiteral("threadRoot"), QStringLiteral("$root"));
        row.insert(QStringLiteral("edited"), true);
        const TimelineEvent e = RoomExportController::eventFromWalkRow(
            row, QStringLiteral("!r:example.org"));
        QCOMPARE(e.type, TimelineEvent::Image);
        QCOMPARE(e.mediaKey, QStringLiteral("export:500:$i"));
        QCOMPARE(e.mediaFilename, QStringLiteral("photo.png"));
        QCOMPARE(e.mediaSize, qint64(42));
        QCOMPARE(e.threadRootId, QStringLiteral("$root"));
        QVERIFY(e.edited);
        QCOMPARE(e.timestamp.toMSecsSinceEpoch(), qint64(5000));
        QVERIFY(e.mediaMxcUrl.isEmpty());

        const TimelineEvent gone = RoomExportController::eventFromWalkRow(
            walkRow(QStringLiteral("$r"), QStringLiteral("redacted"), 1,
                    QStringLiteral("leftover")),
            QStringLiteral("!r:example.org"));
        QVERIFY(gone.redacted);
        QVERIFY(gone.body.isEmpty());
        const TimelineEvent utd = RoomExportController::eventFromWalkRow(
            walkRow(QStringLiteral("$u"), QStringLiteral("undecryptable"), 1),
            QStringLiteral("!r:example.org"));
        QVERIFY(utd.undecryptable);
        const TimelineEvent voice = RoomExportController::eventFromWalkRow(
            walkRow(QStringLiteral("$v"), QStringLiteral("voice"), 1),
            QStringLiteral("!r:example.org"));
        QCOMPARE(voice.type, TimelineEvent::Audio);
        QVERIFY(voice.mediaIsVoice);
    }

    void aFullHtmlExportFetchesByHandleWithinTheBudget()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("out.html"));
        FakeClient client;
        RoomExportController controller;
        controller.setClient(&client);
        QSignalSpy finished(&controller, &RoomExportController::finished);

        RoomExportController::Job job = htmlJob(path);
        job.options.attachmentBudgetBytes = 1000;
        QCOMPARE(controller.start(job), QString());
        QVERIFY(controller.busy());
        QCOMPARE(client.walkMode, 0);
        QCOMPARE(client.walkLimit, qint64(10));

        // A foreign page is ignored.
        client.page(client.walkOp + 77,
                    { walkRow(QStringLiteral("$x"), QStringLiteral("text"), 1,
                              QStringLiteral("FOREIGN")) },
                    true);
        // Newest first, as the walk sends them. "$big" declares more than the
        // budget and must not be fetched at all.
        client.page(client.walkOp,
                    { walkRow(QStringLiteral("$3"), QStringLiteral("text"),
                              3000, QStringLiteral("third")),
                      walkImageRow(QStringLiteral("$big"), 2500, 5000) },
                    false);
        client.page(client.walkOp,
                    { walkImageRow(QStringLiteral("$img"), 2000, 40),
                      walkRow(QStringLiteral("$1"), QStringLiteral("text"),
                              1000, QStringLiteral("first")) },
                    true);

        QTRY_COMPARE(client.fetchedKeys.size(), 1);
        QCOMPARE(client.fetchedKeys.first(), QStringLiteral("export:500:$img"));
        QCOMPARE(controller.phase(), QStringLiteral("attachments"));
        client.ready(client.fetchOps.first(), pngBytes());

        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toBool(), true);
        QVERIFY(!controller.busy());
        // The walk's handles were released.
        QVERIFY(client.cancelledExports.contains(500));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString page = QString::fromUtf8(file.readAll());
        QVERIFY(!page.contains(QStringLiteral("FOREIGN")));
        // Time order, oldest first.
        const qsizetype first = page.indexOf(QStringLiteral("first"));
        const qsizetype third = page.indexOf(QStringLiteral("third"));
        QVERIFY(first > 0 && third > first);
        QVERIFY(page.contains(QString::fromLatin1(pngBytes().toBase64())));
        QVERIFY(page.contains(QStringLiteral("not included: over the size limit")));
        QVERIFY(page.contains(QStringLiteral("Messages in this file: 4")));
        QVERIFY(page.contains(QStringLiteral("the latest 10 messages")));
        QVERIFY(page.contains(QStringLiteral("reaches the start")));
    }

    // A payload the writer refuses (SVG behind an image's name) is never
    // embedded, so it must not use up the size limit either.
    void aRefusedSvgIsNotChargedToTheSizeLimit()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.html"));
        FakeClient client;
        RoomExportController controller;
        controller.setClient(&client);
        QSignalSpy finished(&controller, &RoomExportController::finished);
        RoomExportController::Job job = htmlJob(path);
        // Room for both if nothing is wasted, but not for the PNG once the
        // SVG has been charged: fails on code that charges a refused payload.
        job.options.attachmentBudgetBytes =
            pngBytes().size() + svgBytes().size() - 1;
        QCOMPARE(controller.start(job), QString());
        client.page(client.walkOp,
                    { walkImageRow(QStringLiteral("$png"), 2000, 0),
                      walkImageRow(QStringLiteral("$svg"), 1000, 0) },
                    true);
        // Oldest first: the SVG-bearing row is fetched first.
        QTRY_COMPARE(client.fetchOps.size(), 1);
        QCOMPARE(client.fetchedKeys.first(), QStringLiteral("export:500:$svg"));
        client.ready(client.fetchOps.at(0), svgBytes());
        QTRY_COMPARE(client.fetchOps.size(), 2);
        client.ready(client.fetchOps.at(1), pngBytes());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().at(0).toBool());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString page = QString::fromUtf8(file.readAll());
        QVERIFY(page.contains(QStringLiteral("SVG images are never embedded")));
        QVERIFY2(page.contains(QString::fromLatin1(pngBytes().toBase64())),
                 "the refused SVG used up the size limit");
        QVERIFY(!page.contains(QStringLiteral("over the size limit")));
    }

    void cancelStopsTheWalkAndLeavesNoFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.html"));
        FakeClient client;
        RoomExportController controller;
        controller.setClient(&client);
        QSignalSpy finished(&controller, &RoomExportController::finished);

        QCOMPARE(controller.start(htmlJob(path)), QString());
        const quint64 op = client.walkOp;
        controller.cancel();
        QVERIFY(!controller.busy());
        QVERIFY(client.cancelledExports.contains(op));
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toBool(), false);

        // A page that was already on its way is dropped.
        client.page(op, { walkRow(QStringLiteral("$1"), QStringLiteral("text"),
                                  1, QStringLiteral("late")) },
                    true);
        QTest::qWait(50);
        QCOMPARE(finished.size(), 1);
        QVERIFY2(leftovers(dir).isEmpty(),
                 qPrintable(leftovers(dir).join(QLatin1Char(' '))));
        QVERIFY(client.fetchedKeys.isEmpty());
    }

    void cancelDuringADownloadCancelsItAndLeavesNoFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.html"));
        FakeClient client;
        RoomExportController controller;
        controller.setClient(&client);
        QCOMPARE(controller.start(htmlJob(path)), QString());
        client.page(client.walkOp,
                    { walkImageRow(QStringLiteral("$img"), 2000, 40) }, true);
        QTRY_COMPARE(client.fetchOps.size(), 1);
        // No assertion that the partial file is visible here: on Linux
        // QSaveFile writes an unnamed O_TMPFILE, so nothing is listed while
        // writing; where it falls back to a named temporary, the checks
        // below are what catch a leftover.
        controller.cancel();
        QVERIFY(client.cancelledFetches.contains(client.fetchOps.first()));
        QVERIFY2(leftovers(dir).isEmpty(),
                 qPrintable(leftovers(dir).join(QLatin1Char(' '))));
        // A late answer for the cancelled fetch changes nothing.
        client.ready(client.fetchOps.first(), pngBytes());
        QTest::qWait(50);
        QVERIFY2(leftovers(dir).isEmpty(),
                 qPrintable(leftovers(dir).join(QLatin1Char(' '))));
    }

    void anEncryptedRoomWithoutTheOptInNeverFetches()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.html"));
        FakeClient client;
        RoomExportController controller;
        controller.setClient(&client);
        QSignalSpy finished(&controller, &RoomExportController::finished);
        RoomExportController::Job job = htmlJob(path);
        job.options.encrypted = true;
        job.options.allowEncryptedPlaintext = false;
        QCOMPARE(controller.start(job), QString());
        client.page(client.walkOp,
                    { walkImageRow(QStringLiteral("$img"), 2000, 40),
                      walkRow(QStringLiteral("$1"), QStringLiteral("text"),
                              1000, QStringLiteral("the secret plan")) },
                    true);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().at(0).toBool());
        QVERIFY2(client.fetchedKeys.isEmpty(),
                 "decrypted media was fetched for a file the user did not "
                 "allow to hold it");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString page = QString::fromUtf8(file.readAll());
        QVERIFY(!page.contains(QStringLiteral("the secret plan")));
    }

    void theLoadedRangeNeedsNoWalk()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.txt"));
        FakeClient client;
        client.walkSupported = false;
        RoomExportController controller;
        controller.setClient(&client);
        QSignalSpy finished(&controller, &RoomExportController::finished);
        RoomExportController::Job job = htmlJob(path);
        job.format = roomexport::Format::PlainText;
        job.range = RoomExportController::Range::Loaded;
        job.loadedEvents = { message(QStringLiteral("$1"),
                                     QStringLiteral("hello"), 1000) };
        QCOMPARE(controller.start(job), QString());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().at(0).toBool());
        QCOMPARE(client.walkOp, quint64(0));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        QVERIFY(text.contains(QStringLiteral("hello")));
        QVERIFY(text.contains(QStringLiteral("had loaded")));

        // And a walk range is refused, not silently downgraded, without one.
        job.range = RoomExportController::Range::All;
        QVERIFY(!controller.start(job).isEmpty());
        QVERIFY(!controller.busy());
    }
};

QTEST_MAIN(RoomExportHtmlTest)
#include "RoomExportHtmlTest.moc"
