#include "models/RoomExport.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>

namespace roomexport {
namespace {

// One withheld marker, used everywhere a body is not written. It is a real
// row rather than a gap: the reader of the file has to be able to tell "there
// was a message here that this export does not carry" from "nothing was said".
QString withheldBody()
{
    return QStringLiteral("[message withheld: this room is encrypted and the "
                          "export was made without including its text]");
}

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

// What the file shows for one event's content. NEVER an `mxc:` — a reader of
// the exported file cannot resolve one (authenticated media needs the
// account's token), so printing it offers a live-looking dead link.
QString bodyFor(const TimelineEvent &event, const Options &options)
{
    if (options.encrypted && !options.allowEncryptedPlaintext)
        return withheldBody();
    if (isAttachment(event.type)) {
        const QString name = event.mediaFilename.isEmpty()
            ? event.body : event.mediaFilename;
        const QString label = name.trimmed().isEmpty()
            ? QStringLiteral("attachment") : name.trimmed();
        // The bytes are NOT in this file and the sentence says so, rather
        // than leaving a filename that reads like an enclosure.
        return QStringLiteral("[%1: %2 — not included in this export]")
            .arg(typeName(event.type), label);
    }
    return event.body;
}

QString senderFor(const TimelineEvent &event)
{
    if (!event.senderDisplayName.trimmed().isEmpty())
        return event.senderDisplayName.trimmed();
    return event.sender;
}

} // namespace

bool isExportable(const TimelineEvent &event)
{
    // Virtual rows are presentation, not conversation.
    if (event.isVirtual())
        return false;
    // A local echo has not been sent. Exporting it would put a message in the
    // file that nobody else in the room has, and that may never arrive.
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
        : QLocale().timeFormat(QLocale::ShortFormat);

    QStringList lines;
    lines.append(options.roomName.isEmpty() ? options.roomId
                                            : options.roomName);
    lines.append(options.roomId);
    if (!options.exportedBy.isEmpty()) {
        lines.append(QStringLiteral("Exported by %1 on %2")
                         .arg(options.exportedBy,
                              QDateTime::currentDateTime().toString(
                                  Qt::ISODate)));
    }
    lines.append(QStringLiteral("Messages in this file: %1")
                     .arg(exportableCount(events)));
    // The two limits, stated once and at the top, where a reader of the file
    // sees them before they conclude anything from what is missing.
    lines.append(QStringLiteral(
        "This export carries the messages Lightning had loaded, and no "
        "attachments."));
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
            lines.append(QLocale().toString(lastDate, QLocale::LongFormat));
        }
        const QString stamp = when.isValid()
            ? when.time().toString(timeFormat) : QStringLiteral("--:--");
        const QString body = bodyFor(event, options);
        if (event.type == TimelineEvent::StateChange) {
            // A state row has no author speaking; rendering it as "Name: ..."
            // would attribute a sentence to somebody who never wrote one.
            lines.append(QStringLiteral("[%1] * %2").arg(stamp, body));
            continue;
        }
        if (event.type == TimelineEvent::Emote) {
            lines.append(QStringLiteral("[%1] * %2 %3")
                             .arg(stamp, senderFor(event), body));
            continue;
        }
        // A body with newlines keeps them, indented so the continuation is
        // visibly part of the message above rather than a new one.
        const QStringList bodyLines = body.split(QLatin1Char('\n'));
        lines.append(QStringLiteral("[%1] %2: %3")
                         .arg(stamp, senderFor(event),
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
    // Named in the file itself, so a script reading it cannot mistake a
    // partial export for the room's whole history.
    root.insert(QStringLiteral("scope"),
                QStringLiteral("loaded-timeline"));
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
        if (isAttachment(event.type)) {
            row.insert(QStringLiteral("filename"), event.mediaFilename);
            row.insert(QStringLiteral("mimetype"), event.mediaMimetype);
        }
        rows.append(row);
    }
    root.insert(QStringLiteral("messages"), rows);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString render(const QList<TimelineEvent> &events, const Options &options,
               Format format)
{
    return format == Format::Json ? renderJson(events, options)
                                  : renderPlainText(events, options);
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
    // A LEAF, and nothing that can act like a path — the same rule
    // MediaBridge::sanitizedFileName applies, and for the same reason: a room
    // name is chosen by somebody else, and this string is handed to a file
    // dialog as a suggestion.
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                 QStringLiteral("-"));
    for (QChar &c : base) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char('-');
    }
    // Leading dots make a hidden file on Unix and `..` traverses; neither is
    // what a room called ".." meant.
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
                                                  : QStringLiteral("txt");
    return QStringLiteral("%1-%2.%3").arg(base, stamp, suffix);
}

} // namespace roomexport
