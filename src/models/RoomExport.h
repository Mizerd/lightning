#pragma once

#include "matrix/TimelineEvent.h"

#include <QList>
#include <QString>

/// Rendering a room's LOADED timeline into a file the user asked for.
///
/// # What this is, and what it deliberately is not
///
/// It exports the messages Lightning currently HOLDS for the room, and the
/// surface says so with a count. It does not paginate: walking a room's whole
/// history to build a file is an unbounded job whose only honest progress
/// report is "still going", and a partial export presented as a complete one
/// is a lie about a conversation. Loading more first is the user's own,
/// visible action.
///
/// It carries NO media. An attachment row exports as its filename and type,
/// never as bytes and never as an `mxc:` the file's reader cannot resolve —
/// an authenticated media URL in a text file is a dead link that looks live.
///
/// # Encrypted rooms
///
/// CLAUDE.md §6 keeps encrypted-room plaintext memory-only, and this is the
/// one place that writes it to disk. That makes an export of an encrypted
/// room an EXPLICIT, user-chosen exception rather than something the format
/// happens to allow: the caller must pass `allowEncryptedPlaintext`, the UI
/// must have asked for it in those words, and the file that results is exactly
/// as readable as any other file on that computer. Nothing here weakens the
/// rule anywhere else — the cache still refuses encrypted rows, and this
/// function never writes to a cache.
///
/// Every renderer is PURE (events in, string out). The file writing is the
/// caller's, so the whole shape of the output is unit-testable without a
/// filesystem, and so the one place that touches disk is small enough to read.
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
    /// Required before a single decrypted body is written for an encrypted
    /// room. False renders the bodies as a withheld marker instead of
    /// refusing outright, so a user who declines still gets the shape of the
    /// conversation rather than an error.
    bool allowEncryptedPlaintext = false;
    /// 24-hour when true; otherwise the locale's short time.
    bool use24HourClock = false;
};

/// True when this event contributes a row to an export. Virtual rows (date
/// dividers, the read marker, the timeline-start marker) are presentation, not
/// conversation, and a local echo has not been sent yet.
bool isExportable(const TimelineEvent &event);

/// How many of `events` would appear in a file. This is the number the UI
/// shows, so it must be computed the same way the renderer counts.
int exportableCount(const QList<TimelineEvent> &events);

QString renderPlainText(const QList<TimelineEvent> &events,
                        const Options &options);
QString renderJson(const QList<TimelineEvent> &events, const Options &options);

/// The renderer for `format`.
QString render(const QList<TimelineEvent> &events, const Options &options,
               Format format);

/// A safe leaf filename for the export, WITHOUT a directory and WITHOUT an
/// extension. Derived from the room name; falls back to the room id's
/// localpart, then to "room". Never contains a separator, a drive letter or a
/// leading dot — the same discipline the media-save path uses, for the same
/// reason: this string is handed to a file dialog as a suggestion, and a
/// suggestion that can traverse is a suggestion that will.
QString suggestedFileName(const Options &options, Format format);

} // namespace roomexport
