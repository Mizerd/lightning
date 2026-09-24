#pragma once

#include "matrix/TimelineEvent.h"

#include <QList>
#include <QString>

/// Renders a room's loaded timeline into a file the user asked for.
///
/// It exports only the messages Lightning currently holds, and the surface
/// shows the count; it does not paginate, since a partial export presented as
/// complete misrepresents the conversation. It carries no media: an attachment
/// exports as its filename and type, never bytes or an unresolvable `mxc:`.
///
/// Encrypted rooms: this is the one place encrypted-room plaintext is written
/// to disk, as an explicit user-chosen exception. The caller must pass
/// `allowEncryptedPlaintext` after the UI asked in those words. Nothing here
/// writes to a cache.
///
/// Renderers are pure (events in, string out); the caller writes the file.
namespace roomexport {

enum class Format {
    PlainText,  ///< Readable. Dates, times, senders, bodies.
    Json,       ///< Complete. One object per event, machine-readable.
};

struct Options {
    QString roomName;
    QString roomId;
    /// The account doing the export, for the header. A user id, never a token.
    QString exportedBy;
    bool encrypted = false;
    /// Required before any decrypted body of an encrypted room is written. When
    /// false, bodies render as a withheld marker so the user still gets the
    /// conversation's shape.
    bool allowEncryptedPlaintext = false;
    /// 24-hour when true; otherwise the locale's short time.
    bool use24HourClock = false;
};

/// True when this event contributes a row. Virtual rows (dividers, read
/// marker, timeline start) are presentation, and a local echo is unsent.
bool isExportable(const TimelineEvent &event);

/// How many of `events` appear in a file; the UI shows this, so it must match
/// the renderer's count.
int exportableCount(const QList<TimelineEvent> &events);

QString renderPlainText(const QList<TimelineEvent> &events,
                        const Options &options);
QString renderJson(const QList<TimelineEvent> &events, const Options &options);

/// The renderer for `format`.
QString render(const QList<TimelineEvent> &events, const Options &options,
               Format format);

/// Safe leaf filename for the export, without directory or extension. Derived
/// from the room name, falling back to the room id's localpart, then "room".
/// Never contains a separator, drive letter or leading dot: the room name is
/// chosen by someone else and this is offered to a file dialog.
QString suggestedFileName(const Options &options, Format format);

} // namespace roomexport
