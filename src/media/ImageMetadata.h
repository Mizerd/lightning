#pragma once

#include <QByteArray>
#include <QString>

// Removes metadata (location, or everything) from an image's BYTES before it
// is uploaded. Pure functions over a buffer, no Qt GUI and no I/O, so the
// composer can call them at the one dispatch choke point every attach, paste
// and drop passes through, and a test can feed them hand-built files.
//
// Nothing is re-encoded. Each container is edited at the level of its own
// segments / chunks / boxes:
//   * JPEG  - APPn / COM segments are dropped or edited, the entropy-coded
//             image data is copied byte for byte.
//   * PNG   - ancillary chunks (eXIf, tEXt, iTXt, zTXt, tIME) are dropped or
//             edited; every other chunk is copied verbatim.
//   * WebP  - EXIF / XMP chunks are dropped or edited and the RIFF size and
//             VP8X flags are repaired.
//   * HEIC / AVIF (ISO BMFF) - the Exif item is edited IN PLACE (the file
//             length never changes, so no offset in the file moves).
//
// Orientation is preserved: when everything is removed, a minimal Exif block
// holding only the orientation is written back, because dropping it would show
// a rotated photo sideways. A photo with orientation 1 gets no Exif at all.
//
// Fail-closed: a file recognised as one of those formats that cannot be parsed
// reports Failed, and the caller must not send it unchanged (the user asked
// for the metadata to go). A format this file does not handle (GIF, BMP, SVG,
// anything else) reports Unsupported and is passed through by the caller.
namespace imagemeta {

enum class Mode {
    Keep,            // send the file untouched
    RemoveLocation,  // drop GPS (Exif GPS IFD, GPS tags in XMP); keep the rest
    RemoveAll,       // drop Exif, XMP, IPTC, comments, text chunks
};

// "location" / "all" / "keep". An unknown string is the DEFAULT
// (RemoveLocation), never the nearest value: this is a privacy setting and an
// unreadable value must not switch it off.
// Header-only so SettingsManager (linked into dozens of targets) can use them
// without pulling the byte-level code in.
inline Mode defaultMode() { return Mode::RemoveLocation; }
inline Mode modeFromString(const QString &value)
{
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("all"))
        return Mode::RemoveAll;
    if (v == QLatin1String("keep"))
        return Mode::Keep;
    return defaultMode();
}
inline QString modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Keep: return QStringLiteral("keep");
    case Mode::RemoveAll: return QStringLiteral("all");
    case Mode::RemoveLocation: break;
    }
    return QStringLiteral("location");
}

enum class Status {
    Unchanged,    // recognised, nothing to remove (or Mode::Keep)
    Stripped,     // bytes differ from the input
    Unsupported,  // not a format this handles; pass through
    Failed,       // recognised but could not be processed; do not send as is
};

struct Result {
    Status status = Status::Unchanged;
    QByteArray bytes;  // the bytes to send; equals the input unless Stripped
};

Result strip(const QByteArray &input, Mode mode);

// Inspection helpers (used by the tests and by diagnostics; they read only).
// True when the file carries a GPS block in its Exif, or GPS tags in XMP.
bool containsLocation(const QByteArray &input);
// Exif orientation 1..8, or 0 when absent / unreadable.
int exifOrientation(const QByteArray &input);

}  // namespace imagemeta
