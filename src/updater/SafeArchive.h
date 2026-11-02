#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace updater {

enum class ArchivePathVerdict {
    Safe = 0,
    EmptyName,
    EmbeddedNul,
    ControlCharacter,
    TooLong,
    AbsolutePosix,
    AbsoluteDriveLetter,
    UncPath,
    BackslashSeparator,
    ParentTraversal,
    CurrentDirComponent,
    EmptyComponent,
    ReservedDeviceName,
    TrailingDotOrSpace,
    EscapesRoot,
    InvalidRoot,
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

bool isSafeArchivePath(const QString &entryName, const QString &stagingRoot);

QString resolvedArchiveDestination(const QString &entryName,
                                   const QString &stagingRoot);

// True when any component of `path` between `root` and the leaf is an
// existing symbolic link. Used as a second line of defence during extraction:
// even though symlink ENTRIES are refused, a pre-existing symlink inside the
// staging tree must never be traversed.
bool pathTraversesSymlink(const QString &root, const QString &path);

struct ArchiveLimits {
    int maxEntries = 8192;
    qint64 maxTotalUncompressedBytes = 1024LL * 1024 * 1024;
    qint64 maxEntryUncompressedBytes = 512LL * 1024 * 1024;
    qint64 maxArchiveBytes = 1024LL * 1024 * 1024;
    int maxNameLength = 512;
    int maxPathDepth = 32;
    // A single entry may not expand by more than this factor. Deflate tops out
    // near 1032:1 on pathological input; 200:1 is far above anything a real
    // application bundle produces.
    int maxCompressionRatio = 200;
};

enum class ArchiveError {
    None = 0,
    OpenFailed,
    ArchiveTooLarge,
    NotAZipArchive,
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
    InflateUnavailable,
    InflateFailed,
    WriteFailed,
    DestinationNotEmpty,
    DestinationUnusable,
};

const char *archiveErrorName(ArchiveError error);

struct ArchiveResult {
    ArchiveError error = ArchiveError::None;
    QString message;
    QString offendingEntry;
    int entriesWritten = 0;
    qint64 bytesWritten = 0;

    bool ok() const { return error == ArchiveError::None; }
};

ArchiveResult extractZipSafely(const QString &zipPath,
                               const QString &stagingRoot,
                               const ArchiveLimits &limits = ArchiveLimits());

ArchiveResult listZipEntries(const QString &zipPath,
                             const QString &stagingRoot,
                             QStringList *outNames,
                             const ArchiveLimits &limits = ArchiveLimits());

bool deflateSupportAvailable();

}
