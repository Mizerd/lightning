#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// The on-disk form of a sticker / emoji pack that a user can hand to somebody
// else: one ZIP holding `pack.json` and `images/<file>`.
//
//   pack.json   {"format":"lightning.sticker-pack","version":1,
//                "name":"...","attribution":"...",
//                "images":[{"shortcode":"cat","file":"images/cat.png",
//                           "body":"A cat","usage":"sticker|emoticon|both",
//                           "mimetype":"image/png"}]}
//
// Pure functions: no Matrix, no network. Writing is a stored (uncompressed)
// ZIP, which suits files that are already PNG / WebP / JPEG / GIF and needs no
// zlib. Reading is untrusted input from anywhere: it goes through the
// updater's hardened extractor (path traversal, symlinks, size and ratio
// caps, CRC) into a private directory, and the manifest is then checked
// entry by entry. SVG is refused outright on both sides (CLAUDE.md section 6):
// a pack never carries active content.
namespace stickers::archive {

constexpr int kMaxImages = 512;               // MSC pack cap used by the backend
constexpr qint64 kMaxImageBytes = 4 * 1024 * 1024;  // backend upload cap
constexpr qint64 kMaxManifestBytes = 1024 * 1024;

struct Image {
    QString shortcode;
    QString body;
    QString mimetype;       // advisory; the real type is sniffed from the bytes
    QString usage = QStringLiteral("sticker");  // sticker | emoticon | both
    QByteArray data;        // export: the bytes. Import: empty, see `path`.
    QString path;           // import: the extracted file
};

struct Pack {
    QString name;
    QString attribution;
    QList<Image> images;
};

// File extension (without dot) for raster image bytes, or "" when the bytes
// are not PNG / JPEG / WebP / GIF. SVG and anything else are "".
QString extensionForBytes(const QByteArray &bytes);
QString mimeForExtension(const QString &extension);

// Builds the ZIP. Images whose bytes are not a supported raster image, or are
// empty or over kMaxImageBytes, are skipped and counted in `skipped`.
QByteArray buildArchive(const Pack &pack, int *skipped = nullptr);

enum class ReadError {
    None,
    Unreadable,       // cannot open / not a ZIP / refused by the extractor
    NoManifest,
    BadManifest,      // not JSON, wrong format tag, unsupported version
    NoImages,
};

struct ReadResult {
    ReadError error = ReadError::None;
    Pack pack;
    int refused = 0;  // manifest entries dropped: unsafe, oversized, not raster
};

// Extracts `zipPath` into `workDir` (an existing empty directory the caller
// owns) and returns the validated pack. Image `path`s point inside `workDir`.
ReadResult readArchive(const QString &zipPath, const QString &workDir);

// Matrix v1.19's shortcode grammar, `^[A-Za-z0-9_-]{1,100}$`, case-sensitive
// (rust is_valid_shortcode). The validator for new and renamed keys.
bool isValidShortcode(const QString &key);

// The key a NEW image gets for requested text: the text itself when it is
// already valid, otherwise sanitizeShortcode()'s suggestion (rust
// propose_shortcode).
QString proposeShortcode(const QString &requested);

// Turns arbitrary text (a file name, a body) into a SUGGESTED shortcode (rust
// sanitize_shortcode): illegal runs collapse to one `_`, at most 100
// characters, separators trimmed. Never applied to an existing key.
QString sanitizeShortcode(const QString &raw);

}  // namespace stickers::archive
