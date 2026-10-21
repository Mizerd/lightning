#include "models/RoomExport.h"

#include "matrix/StateRowText.h"
#include "models/RoomExportHtml.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>

namespace roomexport {

// One withheld marker, used wherever a body is not written, so a reader can
// tell "a message is omitted" from "nothing was said".
QString withheldBody()
{
    return QStringLiteral("[message withheld: this room is encrypted and the "
                          "export was made without including its text]");
}

QString placeholderBody(const TimelineEvent &event)
{
    // Said in words rather than left blank: an empty row reads as "nothing
    // was said", which is not what happened.
    if (event.redacted)
        return QStringLiteral("[message deleted]");
    if (event.undecryptable)
        return QStringLiteral("[unable to decrypt this message: this device "
                              "does not have its key]");
    return QString();
}

namespace {

QString typeName(TimelineEvent::Type type)
{
    switch (type) {
    case TimelineEvent::TextMessage: return QStringLiteral("m.text");
    case TimelineEvent::Emote:       return QStringLiteral("m.emote");
    case TimelineEvent::Notice:      return QStringLiteral("m.notice");
    case TimelineEvent::Image:       return QStringLiteral("m.image");
    case TimelineEvent::File:        return QStringLiteral("m.file");
    case TimelineEvent::Video:       return QStringLiteral("m.video");
    case TimelineEvent::Audio:       return QStringLiteral("m.audio");
    case TimelineEvent::Sticker:     return QStringLiteral("m.sticker");
    case TimelineEvent::Poll:        return QStringLiteral("m.poll");
    case TimelineEvent::CallEvent:   return QStringLiteral("m.call");
    case TimelineEvent::StateChange: return QStringLiteral("m.state");
    default:                         return QStringLiteral("m.unknown");
    }
}

bool isAttachment(TimelineEvent::Type type)
{
    return type == TimelineEvent::Image || type == TimelineEvent::File
        || type == TimelineEvent::Video || type == TimelineEvent::Audio
        || type == TimelineEvent::Sticker;
}

} // namespace

// A state row the bridge named is written in the exporting user's app
// language, as the timeline shows it; anything else keeps its body. No bidi
// isolates: they are invisible characters in a text file.
QString stateOrBody(const TimelineEvent &event)
{
    if (event.type == TimelineEvent::StateChange && !event.stateSentence.isEmpty())
        return matrix::preview::stateRowText(event, QString(), /*isolate=*/false);
    return event.body;
}

namespace {

// What the file shows for an event's content. Never an `mxc:`: authenticated
// media cannot be resolved from the file, so it would be a dead link.
QString bodyFor(const TimelineEvent &event, const Options &options)
{
    // Room state (created the room, joined, renamed) is never encrypted: Matrix
    // encrypts message events only, so a row the bridge named with a state
    // sentence is plaintext the server already holds and withholding it only
    // mislabels it as a hidden message. Anything without a sentence, or any
    // non-state row, stays withheld.
    const bool plaintextState = event.type == TimelineEvent::StateChange
                                && !event.stateSentence.isEmpty();
    if (options.encrypted && !options.allowEncryptedPlaintext && !plaintextState)
        return withheldBody();
    const QString placeholder = placeholderBody(event);
    if (!placeholder.isEmpty())
        return placeholder;
    if (isAttachment(event.type)) {
        const QString name = event.mediaFilename.isEmpty()
            ? event.body : event.mediaFilename;
        const QString label = name.trimmed().isEmpty()
            ? QStringLiteral("attachment") : name.trimmed();
        // Say the bytes are not included, rather than leaving a bare filename
        // that reads like an enclosure.
        return QStringLiteral("[%1: %2 — not included in this export]")
            .arg(typeName(event.type), label);
    }
    return stateOrBody(event);
}

// The sender's own words under a piece of media, when they wrote any beyond
// the file's name. Empty for a withheld or placeholder row, so it can never
// reveal what the body would not.
QString captionFor(const TimelineEvent &event, const Options &options)
{
    if (!isAttachment(event.type))
        return {};
    if (options.encrypted && !options.allowEncryptedPlaintext)
        return {};
    if (!placeholderBody(event).isEmpty())
        return {};
    const QString caption = event.body.trimmed();
    const QString name = event.mediaFilename.isEmpty()
        ? QString() : event.mediaFilename.trimmed();
    if (caption.isEmpty() || caption == name)
        return {};
    return caption;
}

QString senderFor(const TimelineEvent &event)
{
    if (!event.senderDisplayName.trimmed().isEmpty())
        return event.senderDisplayName.trimmed();
    return event.sender;
}

} // namespace

bool isAttachment(const TimelineEvent &event)
{
    return isAttachment(event.type);
}

bool isExportable(const TimelineEvent &event)
{
    // Virtual rows are presentation, not conversation.
    if (event.isVirtual())
        return false;
    // A local echo has not been sent and may never arrive; nobody else has it.
    if (event.status != TimelineEvent::Sent)
        return false;
    return true;
}

int exportableCount(const QList<TimelineEvent> &events)
{
    int n = 0;
    for (const TimelineEvent &event : events) {
        if (isExportable(event))
            ++n;
    }
    return n;
}

QString renderPlainText(const QList<TimelineEvent> &events,
                        const Options &options)
{
    const QString timeFormat = options.use24HourClock
        ? QStringLiteral("HH:mm")
        : QStringLiteral("h:mm AP");

    // File content is English whatever the host or UI language is, so the
    // dates and times in it never mix two languages.
    const QLocale fileLocale = QLocale::c();

    QStringList lines;
    lines.append(options.roomName.isEmpty() ? options.roomId
                                            : options.roomName);
    lines.append(options.roomId);
    if (!options.exportedBy.isEmpty()) {
        lines.append(QStringLiteral("Exported by %1 on %2")
                         .arg(options.exportedBy,
                              QDateTime::currentDateTimeUtc().toString(
                                  Qt::ISODate)));
    }
    lines.append(QStringLiteral("Events in this file: %1")
                     .arg(exportableCount(events)));
    // State the two limits at the top, before the reader draws conclusions.
    if (options.rangeDescription.isEmpty()) {
        lines.append(QStringLiteral(
            "This export carries the messages Lightning had loaded, and no "
            "attachments."));
    } else {
        lines.append(QStringLiteral("This export carries %1, and no "
                                    "attachments.")
                         .arg(options.rangeDescription));
        if (options.reachesStartOfRoom)
            lines.append(QStringLiteral(
                "It reaches the start of the room's history this account can "
                "read."));
    }
    if (options.undecryptableCount > 0) {
        lines.append(QStringLiteral("Messages that could not be decrypted: %1")
                         .arg(options.undecryptableCount));
    }
    if (options.encrypted) {
        lines.append(options.allowEncryptedPlaintext
            ? QStringLiteral(
                  "This room is ENCRYPTED. The messages below are its "
                  "decrypted text, written in the clear.")
            : QStringLiteral(
                  "This room is ENCRYPTED and the export was made without "
                  "its text, so only the shape of the conversation is here."));
    }
    lines.append(QString());

    QDate lastDate;
    for (const TimelineEvent &event : events) {
        if (!isExportable(event))
            continue;
        const QDateTime when = event.timestamp.toLocalTime();
        if (when.isValid() && when.date() != lastDate) {
            lastDate = when.date();
            lines.append(QString());
            lines.append(fileLocale.toString(lastDate, QLocale::LongFormat));
        }
        const QString stamp = when.isValid()
            ? fileLocale.toString(when.time(), timeFormat)
            : QStringLiteral("--:--");
        QString body = bodyFor(event, options);
        const QString caption = captionFor(event, options);
        if (!caption.isEmpty())
            body += QLatin1Char('\n') + caption;
        if (event.type == TimelineEvent::StateChange) {
            // State rows have no speaker; "Name: ..." would attribute words to
            // someone.
            lines.append(QStringLiteral("[%1] * %2").arg(stamp, body));
            continue;
        }
        // A thread reply sits in time order among the rest; the marker says
        // it belongs to a thread rather than to the main conversation.
        const QString thread = event.threadRootId.isEmpty()
            ? QString() : QStringLiteral("[in thread] ");
        if (event.type == TimelineEvent::Emote) {
            lines.append(QStringLiteral("[%1] %2* %3 %4")
                             .arg(stamp, thread, senderFor(event), body));
            continue;
        }
        // Multi-line bodies keep their newlines, indented as continuations.
        const QStringList bodyLines = body.split(QLatin1Char('\n'));
        lines.append(QStringLiteral("[%1] %2%3: %4")
                         .arg(stamp, thread, senderFor(event),
                              bodyLines.value(0)));
        for (int i = 1; i < bodyLines.size(); ++i)
            lines.append(QStringLiteral("        ") + bodyLines.at(i));
    }
    lines.append(QString());
    return lines.join(QLatin1Char('\n'));
}

QString renderJson(const QList<TimelineEvent> &events, const Options &options)
{
    QJsonObject root;
    root.insert(QStringLiteral("room_id"), options.roomId);
    root.insert(QStringLiteral("room_name"), options.roomName);
    root.insert(QStringLiteral("exported_by"), options.exportedBy);
    root.insert(QStringLiteral("exported_at"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("encrypted"), options.encrypted);
    root.insert(QStringLiteral("includes_message_text"),
                !options.encrypted || options.allowEncryptedPlaintext);
    // Named in the file so a script cannot mistake a partial export for the
    // whole history.
    root.insert(QStringLiteral("scope"), options.scope);
    if (!options.rangeDescription.isEmpty()) {
        root.insert(QStringLiteral("range"), options.rangeDescription);
        root.insert(QStringLiteral("reaches_start_of_room"),
                    options.reachesStartOfRoom);
    }
    if (options.undecryptableCount >= 0) {
        root.insert(QStringLiteral("undecryptable_count"),
                    options.undecryptableCount);
    }
    root.insert(QStringLiteral("includes_attachments"), false);

    QJsonArray rows;
    for (const TimelineEvent &event : events) {
        if (!isExportable(event))
            continue;
        QJsonObject row;
        row.insert(QStringLiteral("event_id"), event.eventId);
        row.insert(QStringLiteral("type"), typeName(event.type));
        row.insert(QStringLiteral("sender"), event.sender);
        row.insert(QStringLiteral("sender_display_name"),
                   event.senderDisplayName);
        row.insert(QStringLiteral("timestamp_ms"),
                   double(event.timestamp.toMSecsSinceEpoch()));
        row.insert(QStringLiteral("timestamp"),
                   event.timestamp.toUTC().toString(Qt::ISODate));
        row.insert(QStringLiteral("body"), bodyFor(event, options));
        if (event.redacted)
            row.insert(QStringLiteral("redacted"), true);
        if (event.undecryptable)
            row.insert(QStringLiteral("undecryptable"), true);
        if (!event.threadRootId.isEmpty())
            row.insert(QStringLiteral("thread_root"), event.threadRootId);
        if (!event.replyToEventId.isEmpty())
            row.insert(QStringLiteral("in_reply_to"), event.replyToEventId);
        // An attachment's name and type are decrypted content too: withheld
        // with the body when the encrypted-room opt-in was not given.
        if (isAttachment(event.type)
            && (!options.encrypted || options.allowEncryptedPlaintext)) {
            row.insert(QStringLiteral("filename"), event.mediaFilename);
            row.insert(QStringLiteral("mimetype"), event.mediaMimetype);
            const QString caption = captionFor(event, options);
            if (!caption.isEmpty())
                row.insert(QStringLiteral("caption"), caption);
        }
        rows.append(row);
    }
    root.insert(QStringLiteral("messages"), rows);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString render(const QList<TimelineEvent> &events, const Options &options,
               Format format)
{
    switch (format) {
    case Format::Json:
        return renderJson(events, options);
    case Format::Html:
        // No attachments: those are fetched and streamed by the controller.
        return QString::fromUtf8(renderHtml(events, options));
    case Format::PlainText:
        break;
    }
    return renderPlainText(events, options);
}

QString suggestedFileName(const Options &options, Format format)
{
    QString base = options.roomName.trimmed();
    if (base.isEmpty()) {
        // The localpart of `!abc:server`, which is stable and carries no
        // separator of its own.
        base = options.roomId;
        base.remove(QLatin1Char('!'));
        base = base.section(QLatin1Char(':'), 0, 0);
    }
    // A leaf only, as in MediaBridge::sanitizedFileName: the room name is
    // chosen by someone else and is offered to a file dialog.
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                 QStringLiteral("-"));
    for (QChar &c : base) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char('-');
    }
    // Leading dots hide the file on Unix and `..` traverses.
    while (base.startsWith(QLatin1Char('.')))
        base.remove(0, 1);
    base = base.trimmed();
    if (base.size() > 80)
        base = base.left(80).trimmed();
    if (base.isEmpty())
        base = QStringLiteral("room");
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd"));
    const QString suffix = format == Format::Json ? QStringLiteral("json")
        : format == Format::Html                  ? QStringLiteral("html")
                                                  : QStringLiteral("txt");
    return QStringLiteral("%1-%2.%3").arg(base, stamp, suffix);
}

} // namespace roomexport
