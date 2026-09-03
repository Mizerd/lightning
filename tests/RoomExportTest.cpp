// Exporting a room's loaded messages to a file.
//
// The renderers are PURE — events in, string out — which is why this suite
// needs no filesystem and can assert on the exact shape of the output. The
// one thing that touches disk lives in AppController and is four lines.
//
// The case that matters most here is the ENCRYPTED one. CLAUDE.md §6 keeps
// encrypted-room plaintext memory-only, and an export is the single
// deliberate exception: it has to be impossible to get a decrypted body into
// a file without the caller having passed the flag a UI only sets after
// asking in plain words. So that is asserted from both sides — the flag
// withheld produces no body text at all, and the flag given produces it.

#include "models/RoomExport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

namespace {

TimelineEvent message(const QString &id, const QString &sender,
                      const QString &display, const QString &body,
                      qint64 tsMs)
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

roomexport::Options plainRoom()
{
    roomexport::Options o;
    o.roomId = QStringLiteral("!r:example.org");
    o.roomName = QStringLiteral("The Lounge");
    o.exportedBy = QStringLiteral("@me:example.org");
    o.use24HourClock = true;
    return o;
}

} // namespace

class RoomExportTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ── What is a row, and what is not ───────────────────────────────────
    void virtualRowsAndUnsentEchoesAreNotConversation()
    {
        QList<TimelineEvent> events;
        events.append(message(QStringLiteral("$1"),
                              QStringLiteral("@a:example.org"),
                              QStringLiteral("Ann"),
                              QStringLiteral("hello"), 1000));

        TimelineEvent divider;
        divider.type = TimelineEvent::DateDivider;
        events.append(divider);
        TimelineEvent marker;
        marker.type = TimelineEvent::ReadMarker;
        events.append(marker);
        TimelineEvent start;
        start.type = TimelineEvent::TimelineStart;
        events.append(start);

        // A local echo has not been sent. Exporting it would put a message in
        // the file that nobody else in the room has, and that may never
        // arrive.
        TimelineEvent echo = message(QStringLiteral("local:1"),
                                     QStringLiteral("@me:example.org"),
                                     QStringLiteral("Me"),
                                     QStringLiteral("pending"), 2000);
        echo.status = TimelineEvent::Sending;
        events.append(echo);
        TimelineEvent failed = message(QStringLiteral("local:2"),
                                       QStringLiteral("@me:example.org"),
                                       QStringLiteral("Me"),
                                       QStringLiteral("never sent"), 3000);
        failed.status = TimelineEvent::Failed;
        events.append(failed);

        QCOMPARE(roomexport::exportableCount(events), 1);
        const QString text = roomexport::renderPlainText(events, plainRoom());
        QVERIFY(text.contains(QStringLiteral("hello")));
        QVERIFY2(!text.contains(QStringLiteral("pending")),
                 "an unsent local echo reached the file");
        QVERIFY2(!text.contains(QStringLiteral("never sent")),
                 "a failed send reached the file");
        // And the count in the header is the same rule the rows use, so the
        // file cannot claim a number it did not write.
        QVERIFY(text.contains(QStringLiteral("Messages in this file: 1")));
    }

    // ── The encrypted-room exception, from both sides ────────────────────
    void anEncryptedRoomWithholdsEveryBodyUnlessItWasExplicitlyAllowed()
    {
        QList<TimelineEvent> events;
        events.append(message(QStringLiteral("$1"),
                              QStringLiteral("@a:example.org"),
                              QStringLiteral("Ann"),
                              QStringLiteral("the secret plan"), 1000));

        roomexport::Options options = plainRoom();
        options.encrypted = true;
        options.allowEncryptedPlaintext = false;

        const QString withheld = roomexport::renderPlainText(events, options);
        QVERIFY2(!withheld.contains(QStringLiteral("the secret plan")),
                 "an encrypted room's message text reached the file without "
                 "the caller ever asking for it");
        // The row still EXISTS: who spoke and when survives, so the file is
        // an honest record with a hole rather than a silent gap.
        QVERIFY(withheld.contains(QStringLiteral("Ann")));
        QVERIFY(withheld.contains(QStringLiteral("withheld")));
        QVERIFY(withheld.contains(QStringLiteral("ENCRYPTED")));

        const QString json = roomexport::renderJson(events, options);
        QVERIFY2(!json.contains(QStringLiteral("the secret plan")),
                 "the JSON renderer leaks what the text renderer withholds");
        const QJsonObject root =
            QJsonDocument::fromJson(json.toUtf8()).object();
        QCOMPARE(root.value(QStringLiteral("encrypted")).toBool(), true);
        QCOMPARE(root.value(QStringLiteral("includes_message_text")).toBool(),
                 false);

        // With the flag the text is written, which is the whole point of the
        // exception — and the header says so in as many words.
        options.allowEncryptedPlaintext = true;
        const QString allowed = roomexport::renderPlainText(events, options);
        QVERIFY(allowed.contains(QStringLiteral("the secret plan")));
        QVERIFY(allowed.contains(QStringLiteral("decrypted text")));
        const QJsonObject allowedRoot = QJsonDocument::fromJson(
            roomexport::renderJson(events, options).toUtf8()).object();
        QCOMPARE(allowedRoot.value(QStringLiteral("includes_message_text"))
                     .toBool(), true);
    }

    // ── No media, and no dead links ──────────────────────────────────────
    void attachmentsExportAsANameAndNeverAsAnMxc()
    {
        TimelineEvent image = message(QStringLiteral("$img"),
                                      QStringLiteral("@a:example.org"),
                                      QStringLiteral("Ann"),
                                      QStringLiteral("photo.png"), 1000);
        image.type = TimelineEvent::Image;
        image.mediaFilename = QStringLiteral("photo.png");
        image.mediaMxcUrl = QStringLiteral("mxc://example.org/abc123");
        image.mediaMimetype = QStringLiteral("image/png");

        const QString text =
            roomexport::renderPlainText({ image }, plainRoom());
        QVERIFY(text.contains(QStringLiteral("photo.png")));
        QVERIFY2(text.contains(QStringLiteral("not included")),
                 "the file names an attachment without saying its bytes are "
                 "absent, so the name reads like an enclosure");
        // An `mxc:` in a text file is a live-looking dead link: a reader of
        // the file cannot resolve one without the account's token.
        QVERIFY2(!text.contains(QStringLiteral("mxc://")),
                 "an unresolvable media URL reached the file");

        const QString json = roomexport::renderJson({ image }, plainRoom());
        QVERIFY2(!json.contains(QStringLiteral("mxc://")),
                 "the JSON renderer wrote an mxc the reader cannot resolve");
        const QJsonObject row = QJsonDocument::fromJson(json.toUtf8())
            .object().value(QStringLiteral("messages")).toArray()
            .at(0).toObject();
        QCOMPARE(row.value(QStringLiteral("filename")).toString(),
                 QStringLiteral("photo.png"));
        QCOMPARE(row.value(QStringLiteral("mimetype")).toString(),
                 QStringLiteral("image/png"));
    }

    // ── The file says what it is ─────────────────────────────────────────
    //
    // A partial export mistaken for a whole history is the failure this
    // surface has to design against, and the person who reads the file later
    // may not be the person who made it — so both limits are IN the file,
    // not only in the dialog that produced it.
    void theFileStatesItsOwnScope()
    {
        const QList<TimelineEvent> events = {
            message(QStringLiteral("$1"), QStringLiteral("@a:example.org"),
                    QStringLiteral("Ann"), QStringLiteral("hi"), 1000),
        };
        const QString text = roomexport::renderPlainText(events, plainRoom());
        QVERIFY(text.contains(QStringLiteral("The Lounge")));
        QVERIFY(text.contains(QStringLiteral("!r:example.org")));
        QVERIFY(text.contains(QStringLiteral("had loaded")));
        QVERIFY(text.contains(QStringLiteral("no attachments")));

        const QJsonObject root = QJsonDocument::fromJson(
            roomexport::renderJson(events, plainRoom()).toUtf8()).object();
        QCOMPARE(root.value(QStringLiteral("scope")).toString(),
                 QStringLiteral("loaded-timeline"));
        QCOMPARE(root.value(QStringLiteral("includes_attachments")).toBool(),
                 false);
        QCOMPARE(root.value(QStringLiteral("room_id")).toString(),
                 QStringLiteral("!r:example.org"));
    }

    // ── The suggested filename is a LEAF ─────────────────────────────────
    //
    // A room name is chosen by somebody else and this string is handed to a
    // file dialog. The same discipline MediaBridge::sanitizedFileName applies
    // to an attachment name, for the same reason.
    void aRoomNameCannotSuggestAPath()
    {
        roomexport::Options options = plainRoom();
        for (const QString &hostile : {
                 QStringLiteral("../../etc/passwd"),
                 QStringLiteral("..\\..\\windows\\system32"),
                 QStringLiteral("/absolute/name"),
                 QStringLiteral("C:\\Users\\me"),
                 QStringLiteral(".hidden") }) {
            options.roomName = hostile;
            const QString name = roomexport::suggestedFileName(
                options, roomexport::Format::PlainText);
            QVERIFY2(!name.contains(QLatin1Char('/')), qPrintable(name));
            QVERIFY2(!name.contains(QLatin1Char('\\')), qPrintable(name));
            QVERIFY2(!name.contains(QLatin1Char(':')), qPrintable(name));
            QVERIFY2(!name.startsWith(QLatin1Char('.')), qPrintable(name));
            QVERIFY2(name.endsWith(QStringLiteral(".txt")), qPrintable(name));
        }

        // A control character is not a filename either.
        options.roomName = QStringLiteral("a\nb\tc");
        const QString cleaned = roomexport::suggestedFileName(
            options, roomexport::Format::Json);
        QVERIFY(!cleaned.contains(QLatin1Char('\n')));
        QVERIFY(!cleaned.contains(QLatin1Char('\t')));
        QVERIFY(cleaned.endsWith(QStringLiteral(".json")));

        // A nameless room falls back to its id's localpart, not to the id —
        // which starts with '!' and carries a ':'.
        options.roomName = QString();
        const QString fallback = roomexport::suggestedFileName(
            options, roomexport::Format::PlainText);
        QVERIFY(fallback.startsWith(QStringLiteral("r-")));
        QVERIFY(!fallback.contains(QLatin1Char('!')));

        // And a room named entirely out of forbidden characters still yields
        // a usable name rather than a bare extension.
        options.roomId = QStringLiteral("!:");
        options.roomName = QStringLiteral("///");
        const QString last = roomexport::suggestedFileName(
            options, roomexport::Format::PlainText);
        QVERIFY2(!last.startsWith(QLatin1Char('.')), qPrintable(last));
        QVERIFY2(last.size() > 4, qPrintable(last));
    }
};

QTEST_MAIN(RoomExportTest)
#include "RoomExportTest.moc"
