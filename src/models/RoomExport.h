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
    Html,       ///< One self-contained page; may embed attachments
                ///< (RoomExportHtml.h).
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

    // ── What the file covers. The defaults describe the loaded timeline. ──
    /// Machine-readable scope: "loaded-timeline", "latest", "from-beginning"
    /// or "date-range". Written into JSON as `scope`.
    QString scope = QStringLiteral("loaded-timeline");
    /// Human-readable range, e.g. "the latest 100 messages". Empty for the
    /// loaded timeline, whose own sentence is used.
    QString rangeDescription;
    /// The walk reached the start of the room's accessible history.
    bool reachesStartOfRoom = false;
    /// Messages present but unreadable (no key). -1 when not counted.
    int undecryptableCount = -1;
    /// HTML only: attachments are embedded, up to this many bytes in total.
    bool includeAttachments = false;
    qint64 attachmentBudgetBytes = 0;
};

/// True when this event contributes a row. Virtual rows (dividers, read
/// marker, timeline start) are presentation, and a local echo is unsent.
bool isExportable(const TimelineEvent &event);

/// True for the event types that carry a file (image, video, audio, file,
/// sticker).
bool isAttachment(const TimelineEvent &event);

/// The sentence a file uses for a row whose text is not written: an encrypted
/// room exported without its text.
QString withheldBody();

/// How many of `events` appear in a file; the UI shows this, so it must match
/// the renderer's count.
int exportableCount(const QList<TimelineEvent> &events);

QString renderPlainText(const QList<TimelineEvent> &events,
                        const Options &options);
QString renderJson(const QList<TimelineEvent> &events, const Options &options);

/// The renderer for `format`. Html goes through RoomExportHtml.h without
/// attachments.
QString render(const QList<TimelineEvent> &events, const Options &options,
               Format format);

/// What a row says instead of content it has none of: a deleted message, an
/// undecryptable one. Empty for an ordinary row.
QString placeholderBody(const TimelineEvent &event);

// A state row's text for the file: the sentence the bridge named, in the
// exporting user's app language (the timeline's renderer), else the body.
QString stateOrBody(const TimelineEvent &event);

/// Safe leaf filename for the export, without directory or extension. Derived
/// from the room name, falling back to the room id's localpart, then "room".
/// Never contains a separator, drive letter or leading dot: the room name is
/// chosen by someone else and this is offered to a file dialog.
QString suggestedFileName(const Options &options, Format format);

} // namespace roomexport
