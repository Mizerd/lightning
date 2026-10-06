#pragma once

// File names for saving an attachment, and what may be opened afterwards.
//
// Pure functions, no state: the download flow (DownloadsController), the save
// dialog's prefill and the "Open" / "Show in folder" actions all ask these the
// same questions, and the answers are tested in one place.
//
// Every name that reaches here is SENDER-CHOSEN (an event's `filename` or
// `body`), so it is reduced to one harmless leaf before anything else.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace savenaming {

/// The sender's name reduced to a single leaf that is valid on every platform
/// this app ships on: no directory parts, no control characters, none of
/// Windows' `<>:"|?*`, no leading dots (a hidden file in Downloads is a
/// surprise) and no trailing dots or spaces (Windows strips them, which would
/// turn `x.exe.` into `x.exe` behind the risk check). Bounded length, the
/// extension kept. Empty when nothing usable is left.
QString sanitizeLeaf(const QString &raw);

/// The usual extension for a MIME type, lower-case and without the dot, from a
/// fixed table first (deterministic across platforms and MIME databases) and
/// the platform's MIME database second. Empty for an unknown or generic type
/// (`application/octet-stream`), which has no extension to give.
QString extensionForMime(const QString &mime);

/// Whether `suffix` (no dot) is shaped like an extension at all: 1-10
/// characters, letters/digits/`+`/`-`/`_`, at least one letter. `06` in
/// "Screenshot 2026.10.06" is not.
bool looksLikeExtension(const QString &suffix);

/// The extension `leaf` already carries, lower-case and without the dot, or
/// empty when it has none that is known: a suffix counts only when it is
/// shaped like one AND is a known extension (the table above, or a glob the
/// MIME database knows). Compound archive suffixes (`tar.gz`) come back
/// whole.
QString extensionOf(const QString &leaf);

/// The name to save `rawName` under, given the event's MIME type: sanitized,
/// with the MIME type's extension appended when the name has no known one.
/// Never empty: a missing name becomes "image" / "video" / "audio" / "file"
/// by the MIME family, plus the extension.
QString suggestedFileName(const QString &rawName, const QString &mime);

/// A name the user typed in a save dialog, with `expectedExtension` appended
/// when they removed every extension. A DIFFERENT extension is their choice
/// and is kept; dropping it silently is what this exists to prevent.
QString ensureExtension(const QString &chosenLeaf,
                        const QString &expectedExtension);

/// "name (n).ext" — the browser convention Element follows. `n <= 0` returns
/// the leaf unchanged. A compound archive suffix stays together:
/// "logs (1).tar.gz".
QString numberedName(const QString &leaf, int n);

/// The first of `leaf`, "leaf (1)", "leaf (2)", ... that does not exist in
/// `directory`. Advisory only: the writer must still create the file
/// exclusively, because another process can take the name in between.
QString uniqueFileName(const QString &directory, const QString &leaf);

/// Whether opening this file could run a program or a script: executables,
/// installers, scripts, shortcuts and launchers, by extension (what every
/// desktop decides by when asked to open a file) and by the sender's MIME
/// type. A file with no extension at all is risky too, because the desktop
/// then decides by content. Such files are never opened from Lightning; the
/// download notice offers "Show in folder" only.
bool isRiskyToOpen(const QString &fileName, const QString &mime = {});

/// The mark-of-the-web every downloaded file carries on Windows:
/// "[ZoneTransfer]\r\nZoneId=3\r\n" (Internet zone), as browsers write it,
/// with no URL in it.
QByteArray zoneIdentifierContent();

/// Windows: writes zoneIdentifierContent() to `path`'s Zone.Identifier
/// alternate data stream, so SmartScreen and Office's Protected View treat
/// the file as downloaded. Best effort; false when the volume has no streams.
/// Other platforms: does nothing and returns false.
bool markAsDownloaded(const QString &path);

/// Save-dialog filters for `leaf`: "<EXT> file (*.<ext>)" first, so the
/// dialog shows what the file is, then "All files (*)". Only the latter
/// when the name has no extension.
QStringList saveDialogFilters(const QString &leaf);

} // namespace savenaming
