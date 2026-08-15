#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

// Hardened ZIP extraction for the windows-portable update path.
//
// WHY A HAND-WRITTEN READER. Qt has no public ZIP reader, and delegating to a
// system `unzip` would mean (a) trusting whatever binary happens to be first
// on PATH, (b) handing it a path we cannot re-validate, and (c) losing all
// per-entry control over traversal, symlinks and size caps — the exact checks
// UPDATE-SPEC §10 makes mandatory. So this module parses the central
// directory itself and decides every destination path in our own code.
//
// The parser is deliberately narrow. It supports exactly what Lightning's own
// portable ZIP uses — stored (method 0) and deflate (method 8), no ZIP64, no
// encryption, no multi-disk — and REJECTS everything else rather than
// guessing. A refusal is always safe here: the user can still install
// manually.
//
// Everything the reader trusts comes from the central directory, never from a
// local file header, because the two can disagree and only the central
// directory is authoritative.

namespace updater {

// ---------------------------------------------------------------------------
// Path safety — the pure, exhaustively testable core.
// ---------------------------------------------------------------------------

enum class ArchivePathVerdict {
    Safe = 0,
    EmptyName,
    EmbeddedNul,
    ControlCharacter,
    TooLong,
    AbsolutePosix,      // "/etc/passwd"
    AbsoluteDriveLetter, // "C:\x", "c:/x"
    UncPath,            // "\\\\server\\share", "//server/share"
    BackslashSeparator, // any '\' — ZIP names must use '/' only
    ParentTraversal,    // any ".." path component
    CurrentDirComponent, // any "." path component
    EmptyComponent,     // "a//b"
    ReservedDeviceName, // CON, PRN, AUX, NUL, COM1..9, LPT1..9
    TrailingDotOrSpace, // "a." / "a " — Windows silently strips these
    EscapesRoot,        // canonicalised destination leaves the staging root
    InvalidRoot,        // staging root is empty / not absolute
};

const char *archivePathVerdictName(ArchivePathVerdict verdict);

// Decides whether `entryName` (a raw ZIP central-directory name) may be
// extracted underneath `stagingRoot`.
//
// Rejects, in order: empty names, embedded NULs, control characters,
// over-long names, absolute paths in POSIX / drive-letter / UNC form, any
// backslash, any ".." or "." component, empty components, Windows reserved
// device names, components with a trailing dot or space, and finally any name
// whose cleaned destination does not sit strictly underneath the cleaned
// staging root.
//
// `stagingRoot` must be absolute. It does NOT need to exist — when it does,
// its canonical form (symlinks resolved) is used as the containment boundary,
// which is what makes a symlinked staging directory safe.
ArchivePathVerdict checkArchivePath(const QString &entryName,
                                    const QString &stagingRoot);

// Convenience predicate with the signature named in the task contract.
bool isSafeArchivePath(const QString &entryName, const QString &stagingRoot);

// The absolute destination for a name already accepted by checkArchivePath.
// Returns an empty string for anything that is not Safe.
QString resolvedArchiveDestination(const QString &entryName,
                                   const QString &stagingRoot);

// True when any component of `path` between `root` and the leaf is an
// existing symbolic link. Used as a second line of defence during extraction:
// even though symlink ENTRIES are refused, a pre-existing symlink inside the
// staging tree must never be traversed.
bool pathTraversesSymlink(const QString &root, const QString &path);

// ---------------------------------------------------------------------------
// Caps (zip-bomb guard)
// ---------------------------------------------------------------------------

struct ArchiveLimits {
    int maxEntries = 8192;
    qint64 maxTotalUncompressedBytes = 1024LL * 1024 * 1024; // 1 GiB
    qint64 maxEntryUncompressedBytes = 512LL * 1024 * 1024;  // 512 MiB
    qint64 maxArchiveBytes = 1024LL * 1024 * 1024;           // 1 GiB on disk
    int maxNameLength = 512;
    int maxPathDepth = 32;
    // A single entry may not expand by more than this factor. Deflate tops out
    // near 1032:1 on pathological input; 200:1 is far above anything a real
    // application bundle produces.
    int maxCompressionRatio = 200;
};

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

enum class ArchiveError {
    None = 0,
    OpenFailed,
    ArchiveTooLarge,
    NotAZipArchive,       // no end-of-central-directory record
    Truncated,
    UnsupportedZip64,
    UnsupportedMultiDisk,
    UnsupportedEncryption,
    UnsupportedCompression,
    SymlinkEntryRejected,
    UnsafeEntryPath,
    TooManyEntries,
    TotalSizeExceeded,
    EntrySizeExceeded,
    CompressionRatioExceeded,
    ChecksumMismatch,
    InflateUnavailable,   // built without zlib and the entry is deflated
    InflateFailed,
    WriteFailed,
    DestinationNotEmpty,
    DestinationUnusable,
};

const char *archiveErrorName(ArchiveError error);

struct ArchiveResult {
    ArchiveError error = ArchiveError::None;
    QString message;      // never contains file contents
    QString offendingEntry; // raw entry name, truncated; may be empty
    int entriesWritten = 0;
    qint64 bytesWritten = 0;

    bool ok() const { return error == ArchiveError::None; }
};

// Extracts `zipPath` into `stagingRoot`, which must be an absolute path to an
// existing, EMPTY directory that the helper itself created. Any refusal
// leaves the staging root in whatever partial state extraction reached — the
// caller (main.cpp) deletes the whole staging directory on failure and only
// ever promotes a fully successful extraction.
ArchiveResult extractZipSafely(const QString &zipPath,
                               const QString &stagingRoot,
                               const ArchiveLimits &limits = ArchiveLimits());

// Lists entry names without writing anything. Useful for layout validation
// and for tests; applies the same per-entry safety verdict.
ArchiveResult listZipEntries(const QString &zipPath,
                             const QString &stagingRoot,
                             QStringList *outNames,
                             const ArchiveLimits &limits = ArchiveLimits());

// True when this build can inflate deflated entries. Stored entries always
// work. See LIGHTNING_UPDATER_HAVE_ZLIB in the CMake target definition.
bool deflateSupportAvailable();

} // namespace updater
