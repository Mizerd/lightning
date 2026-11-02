
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

}

class RoomExportTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aCaptionIsKeptAndTheHeaderDatesAreEnglish()
    {
        const QLocale before;
        QLocale::setDefault(QLocale(QLocale::Lithuanian));
        TimelineEvent image = message(QStringLiteral("$i"),
                                      QStringLiteral("@a:example.org"),
                                      QStringLiteral("Ann"),
                                      QStringLiteral("sunset picnic"),
                                      1760400000000);
        image.type = TimelineEvent::Image;
        image.mediaFilename = QStringLiteral("sunset.png");
        QList<TimelineEvent> events{image};

        const QString text = roomexport::renderPlainText(events, plainRoom());
        QVERIFY2(text.contains(QStringLiteral("sunset picnic")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("October")), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("spalio")));

        const QString json = roomexport::renderJson(events, plainRoom());
        const QJsonObject row = QJsonDocument::fromJson(json.toUtf8())
            .object().value(QStringLiteral("messages")).toArray().at(0).toObject();
        QCOMPARE(row.value(QStringLiteral("caption")).toString(),
                 QStringLiteral("sunset picnic"));

        events[0].body = QStringLiteral("sunset.png");
        QVERIFY(!roomexport::renderJson(events, plainRoom())
                     .contains(QStringLiteral("\"caption\"")));

        events[0].body = QStringLiteral("sunset picnic");
        roomexport::Options enc = plainRoom();
        enc.encrypted = true;
        QVERIFY(!roomexport::renderPlainText(events, enc)
                     .contains(QStringLiteral("sunset picnic")));
        QVERIFY(!roomexport::renderJson(events, enc)
                     .contains(QStringLiteral("sunset picnic")));
        QLocale::setDefault(before);
    }

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
        QVERIFY(text.contains(QStringLiteral("Events in this file: 1")));
    }

    void aNamedStateRowIsNotWithheldInAnEncryptedRoom()
    {
        TimelineEvent created;
        created.eventId = QStringLiteral("$s");
        created.sender = QStringLiteral("@a:example.org");
        created.senderDisplayName = QStringLiteral("Ann");
        created.type = TimelineEvent::StateChange;
        created.stateSentence = QStringLiteral("room_created");
        created.body = QStringLiteral("Ann created the room.");
        created.timestamp = QDateTime::fromMSecsSinceEpoch(1000, Qt::UTC);
        TimelineEvent unnamed = created;
        unnamed.eventId = QStringLiteral("$u");
        unnamed.stateSentence.clear();
        unnamed.body = QStringLiteral("opaque-body-text");
        QList<TimelineEvent> events{ created, unnamed,
            message(QStringLiteral("$1"), QStringLiteral("@a:example.org"),
                    QStringLiteral("Ann"), QStringLiteral("the secret plan"), 2000) };
        roomexport::Options options = plainRoom();
        options.encrypted = true;
        options.allowEncryptedPlaintext = false;
        const QString text = roomexport::renderPlainText(events, options);
        QVERIFY(!text.contains(QStringLiteral("opaque-body-text")));
        QVERIFY(!text.contains(QStringLiteral("the secret plan")));
        QVERIFY2(text.count(QStringLiteral("[message withheld")) == 2,
                 qPrintable(text));
    }

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

        options.allowEncryptedPlaintext = true;
        const QString allowed = roomexport::renderPlainText(events, options);
        QVERIFY(allowed.contains(QStringLiteral("the secret plan")));
        QVERIFY(allowed.contains(QStringLiteral("decrypted text")));
        const QJsonObject allowedRoot = QJsonDocument::fromJson(
            roomexport::renderJson(events, options).toUtf8()).object();
        QCOMPARE(allowedRoot.value(QStringLiteral("includes_message_text"))
                     .toBool(), true);
    }

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

    void anEncryptedRoomsAttachmentNameIsWithheldInJson()
    {
        TimelineEvent image = message(QStringLiteral("$img"),
                                      QStringLiteral("@a:example.org"),
                                      QStringLiteral("Ann"),
                                      QStringLiteral("secret-plan.pdf"), 1000);
        image.type = TimelineEvent::File;
        image.mediaFilename = QStringLiteral("secret-plan.pdf");
        image.mediaMimetype = QStringLiteral("application/pdf");

        roomexport::Options options = plainRoom();
        options.encrypted = true;
        options.allowEncryptedPlaintext = false;
        const QString json = roomexport::renderJson({ image }, options);
        QVERIFY2(!json.contains(QStringLiteral("secret-plan")),
                 "an encrypted room's attachment name reached the file "
                 "without the opt-in");
        QVERIFY(!json.contains(QStringLiteral("application/pdf")));
        const QJsonObject row = QJsonDocument::fromJson(json.toUtf8())
            .object().value(QStringLiteral("messages")).toArray()
            .at(0).toObject();
        QVERIFY(!row.contains(QStringLiteral("filename")));
        QVERIFY(!row.contains(QStringLiteral("mimetype")));
        QVERIFY(!roomexport::renderPlainText({ image }, options)
                     .contains(QStringLiteral("secret-plan")));

        options.allowEncryptedPlaintext = true;
        const QJsonObject allowed = QJsonDocument::fromJson(
            roomexport::renderJson({ image }, options).toUtf8())
            .object().value(QStringLiteral("messages")).toArray()
            .at(0).toObject();
        QCOMPARE(allowed.value(QStringLiteral("filename")).toString(),
                 QStringLiteral("secret-plan.pdf"));
    }

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

        options.roomName = QStringLiteral("a\nb\tc");
        const QString cleaned = roomexport::suggestedFileName(
            options, roomexport::Format::Json);
        QVERIFY(!cleaned.contains(QLatin1Char('\n')));
        QVERIFY(!cleaned.contains(QLatin1Char('\t')));
        QVERIFY(cleaned.endsWith(QStringLiteral(".json")));

        options.roomName = QString();
        const QString fallback = roomexport::suggestedFileName(
            options, roomexport::Format::PlainText);
        QVERIFY(fallback.startsWith(QStringLiteral("r-")));
        QVERIFY(!fallback.contains(QLatin1Char('!')));

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
