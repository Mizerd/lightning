#include "updater/SafeArchive.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>

#include <cstring>
#include <functional>
#include <utility>

#if defined(LIGHTNING_UPDATER_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace updater {
namespace {

// --- little-endian readers -------------------------------------------------

quint16 readU16(const QByteArray &data, int offset)
{
    return static_cast<quint16>(static_cast<quint8>(data.at(offset)))
         | (static_cast<quint16>(static_cast<quint8>(data.at(offset + 1))) << 8);
}

quint32 readU32(const QByteArray &data, int offset)
{
    return static_cast<quint32>(static_cast<quint8>(data.at(offset)))
         | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 8)
         | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 16)
         | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 3))) << 24);
}

// --- CRC-32 (IEEE), implemented locally so stored entries verify even in a
// build without zlib. This is a checksum, not a security primitive: it proves
// the archive is intact, never that it is authentic. Authenticity is the
// Ed25519 manifest signature plus the SHA-256 the downloader already checked
// before the helper ever saw this file. ------------------------------------

quint32 crc32Update(quint32 crc, const char *data, qsizetype length)
{
    static quint32 table[256];
    static bool initialised = false;
    if (!initialised) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        initialised = true;
    }
    crc ^= 0xFFFFFFFFu;
    for (qsizetype i = 0; i < length; ++i)
        crc = table[(crc ^ static_cast<quint8>(data[i])) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// --- path helpers ----------------------------------------------------------

constexpr int kHardMaxNameLength = 512;
constexpr int kHardMaxPathDepth = 32;

bool isReservedDeviceComponent(const QString &component)
{
    const int dot = component.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? component : component.left(dot)).toUpper();
    static const QStringList kReserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"),
    };
    if (kReserved.contains(stem))
        return true;
    if (stem.size() == 4
        && (stem.startsWith(QLatin1String("COM"))
            || stem.startsWith(QLatin1String("LPT")))) {
        const QChar digit = stem.at(3);
        return digit >= QLatin1Char('1') && digit <= QLatin1Char('9');
    }
    return false;
}

QString containmentRoot(const QString &stagingRoot)
{
    const QFileInfo info(stagingRoot);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return QDir::cleanPath(canonical);
    return QDir::cleanPath(info.absoluteFilePath());
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

struct CentralEntry {
    QString name;
    quint16 flags = 0;
    quint16 method = 0;
    quint32 crc = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 localHeaderOffset = 0;
    quint16 versionMadeBy = 0;
    quint32 externalAttributes = 0;

    bool isDirectory() const
    {
        return name.endsWith(QLatin1Char('/'))
            || ((externalAttributes & 0x10u) != 0 && uncompressedSize == 0);
    }
    quint16 unixMode() const
    {
        if ((versionMadeBy >> 8) != 3) // 3 == Unix
            return 0;
        return static_cast<quint16>((externalAttributes >> 16) & 0xFFFFu);
    }
    bool isSymlink() const { return (unixMode() & 0xF000u) == 0xA000u; }
    bool isRegularOrUnspecified() const
    {
        const quint16 kind = unixMode() & 0xF000u;
        return kind == 0 || kind == 0x8000u || kind == 0x4000u;
    }
    bool isExecutable() const { return (unixMode() & 0111u) != 0; }
};

ArchiveResult archiveFail(ArchiveError error, const QString &message,
                          const QString &entry = QString())
{
    ArchiveResult result;
    result.error = error;
    result.message = message;
    result.offendingEntry = entry.left(256);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Verdict names
// ---------------------------------------------------------------------------

const char *archivePathVerdictName(ArchivePathVerdict verdict)
{
    switch (verdict) {
    case ArchivePathVerdict::Safe: return "safe";
    case ArchivePathVerdict::EmptyName: return "empty-name";
    case ArchivePathVerdict::EmbeddedNul: return "embedded-nul";
    case ArchivePathVerdict::ControlCharacter: return "control-character";
    case ArchivePathVerdict::TooLong: return "too-long";
    case ArchivePathVerdict::AbsolutePosix: return "absolute-posix";
    case ArchivePathVerdict::AbsoluteDriveLetter: return "absolute-drive-letter";
    case ArchivePathVerdict::UncPath: return "unc-path";
    case ArchivePathVerdict::BackslashSeparator: return "backslash-separator";
    case ArchivePathVerdict::ParentTraversal: return "parent-traversal";
    case ArchivePathVerdict::CurrentDirComponent: return "current-dir-component";
    case ArchivePathVerdict::EmptyComponent: return "empty-component";
    case ArchivePathVerdict::ReservedDeviceName: return "reserved-device-name";
    case ArchivePathVerdict::TrailingDotOrSpace: return "trailing-dot-or-space";
    case ArchivePathVerdict::EscapesRoot: return "escapes-root";
    case ArchivePathVerdict::InvalidRoot: return "invalid-root";
    }
    return "unknown";
}

const char *archiveErrorName(ArchiveError error)
{
    switch (error) {
    case ArchiveError::None: return "none";
    case ArchiveError::OpenFailed: return "open-failed";
    case ArchiveError::ArchiveTooLarge: return "archive-too-large";
    case ArchiveError::NotAZipArchive: return "not-a-zip-archive";
    case ArchiveError::Truncated: return "truncated";
    case ArchiveError::UnsupportedZip64: return "unsupported-zip64";
    case ArchiveError::UnsupportedMultiDisk: return "unsupported-multi-disk";
    case ArchiveError::UnsupportedEncryption: return "unsupported-encryption";
    case ArchiveError::UnsupportedCompression: return "unsupported-compression";
    case ArchiveError::SymlinkEntryRejected: return "symlink-entry-rejected";
    case ArchiveError::UnsafeEntryPath: return "unsafe-entry-path";
    case ArchiveError::TooManyEntries: return "too-many-entries";
    case ArchiveError::TotalSizeExceeded: return "total-size-exceeded";
    case ArchiveError::EntrySizeExceeded: return "entry-size-exceeded";
    case ArchiveError::CompressionRatioExceeded: return "compression-ratio-exceeded";
    case ArchiveError::ChecksumMismatch: return "checksum-mismatch";
    case ArchiveError::InflateUnavailable: return "inflate-unavailable";
    case ArchiveError::InflateFailed: return "inflate-failed";
    case ArchiveError::WriteFailed: return "write-failed";
    case ArchiveError::DestinationNotEmpty: return "destination-not-empty";
    case ArchiveError::DestinationUnusable: return "destination-unusable";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// checkArchivePath — the pure core
// ---------------------------------------------------------------------------

ArchivePathVerdict checkArchivePath(const QString &entryName,
                                    const QString &stagingRoot)
{
    if (stagingRoot.isEmpty() || !QFileInfo(stagingRoot).isAbsolute())
        return ArchivePathVerdict::InvalidRoot;

    if (entryName.isEmpty())
        return ArchivePathVerdict::EmptyName;

    for (const QChar ch : entryName) {
        if (ch.unicode() == 0)
            return ArchivePathVerdict::EmbeddedNul;
    }
    for (const QChar ch : entryName) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7F)
            return ArchivePathVerdict::ControlCharacter;
    }
    if (entryName.size() > kHardMaxNameLength)
        return ArchivePathVerdict::TooLong;

    // UNC forms, in both separator styles, before any other absolute check.
    if (entryName.startsWith(QLatin1String("//"))
        || entryName.startsWith(QLatin1String("\\\\")))
        return ArchivePathVerdict::UncPath;

    // Drive-letter absolute, e.g. "C:\x", "c:/x", and even the drive-relative
    // "C:x" form, which Windows resolves against that drive's current
    // directory — equally unacceptable.
    if (entryName.size() >= 2 && entryName.at(1) == QLatin1Char(':')
        && entryName.at(0).isLetter())
        return ArchivePathVerdict::AbsoluteDriveLetter;

    if (entryName.startsWith(QLatin1Char('/')))
        return ArchivePathVerdict::AbsolutePosix;
    if (entryName.startsWith(QLatin1Char('\\')))
        return ArchivePathVerdict::BackslashSeparator;

    // ZIP names use '/' exclusively. A backslash anywhere is either a
    // Windows-style separator smuggled past a '/'-only check, or a literal
    // backslash in a filename — both are refused rather than interpreted.
    if (entryName.contains(QLatin1Char('\\')))
        return ArchivePathVerdict::BackslashSeparator;

    QString normalised = entryName;
    if (normalised.endsWith(QLatin1Char('/')))
        normalised.chop(1); // a directory entry's trailing separator
    if (normalised.isEmpty())
        return ArchivePathVerdict::EmptyName;

    const QStringList components = normalised.split(QLatin1Char('/'));
    if (components.size() > kHardMaxPathDepth)
        return ArchivePathVerdict::TooLong;

    for (const QString &component : components) {
        if (component.isEmpty())
            return ArchivePathVerdict::EmptyComponent;
        if (component == QLatin1String(".."))
            return ArchivePathVerdict::ParentTraversal;
        if (component == QLatin1String("."))
            return ArchivePathVerdict::CurrentDirComponent;
        if (component.endsWith(QLatin1Char('.'))
            || component.endsWith(QLatin1Char(' ')))
            return ArchivePathVerdict::TrailingDotOrSpace;
        if (isReservedDeviceComponent(component))
            return ArchivePathVerdict::ReservedDeviceName;
    }

    // Final containment check against the (canonicalised where possible)
    // staging root. Everything above already forbids the constructs that make
    // this escape, so a failure here means a case we did not anticipate —
    // which is exactly why the check stays.
    const QString root = containmentRoot(stagingRoot);
    if (root.isEmpty())
        return ArchivePathVerdict::InvalidRoot;
    const QString destination =
        QDir::cleanPath(root + QLatin1Char('/') + normalised);
    const QString prefix = root.endsWith(QLatin1Char('/'))
                               ? root
                               : root + QLatin1Char('/');
    if (destination.size() <= prefix.size()
        || !destination.startsWith(prefix, pathCaseSensitivity()))
        return ArchivePathVerdict::EscapesRoot;

    return ArchivePathVerdict::Safe;
}

bool isSafeArchivePath(const QString &entryName, const QString &stagingRoot)
{
    return checkArchivePath(entryName, stagingRoot) == ArchivePathVerdict::Safe;
}

QString resolvedArchiveDestination(const QString &entryName,
                                   const QString &stagingRoot)
{
    if (checkArchivePath(entryName, stagingRoot) != ArchivePathVerdict::Safe)
        return QString();
    QString normalised = entryName;
    if (normalised.endsWith(QLatin1Char('/')))
        normalised.chop(1);
    return QDir::cleanPath(containmentRoot(stagingRoot) + QLatin1Char('/')
                           + normalised);
}

bool pathTraversesSymlink(const QString &root, const QString &path)
{
    const QString cleanRoot = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    const QString cleanPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString prefix = cleanRoot.endsWith(QLatin1Char('/'))
                               ? cleanRoot
                               : cleanRoot + QLatin1Char('/');
    if (!cleanPath.startsWith(prefix, pathCaseSensitivity()))
        return true; // outside the root at all: treat as unsafe

    const QString relative = cleanPath.mid(prefix.size());
    QString walked = cleanRoot;
    const QStringList components = relative.split(QLatin1Char('/'),
                                                  Qt::SkipEmptyParts);
    for (const QString &component : components) {
        walked += QLatin1Char('/');
        walked += component;
        const QFileInfo info(walked);
        if (info.isSymLink())
            return true;
    }
    return false;
}

bool deflateSupportAvailable()
{
#if defined(LIGHTNING_UPDATER_HAVE_ZLIB)
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// ZIP reading
// ---------------------------------------------------------------------------

namespace {

struct CentralDirectory {
    QList<CentralEntry> entries;
    ArchiveResult failure; // ok() when parsing succeeded
};

CentralDirectory readCentralDirectory(QFile &file, const ArchiveLimits &limits)
{
    CentralDirectory out;

    const qint64 fileSize = file.size();
    if (fileSize < 22) {
        out.failure = archiveFail(ArchiveError::NotAZipArchive,
                                  QStringLiteral("file is too small to be a ZIP"));
        return out;
    }
    if (fileSize > limits.maxArchiveBytes) {
        out.failure = archiveFail(ArchiveError::ArchiveTooLarge,
                                  QStringLiteral("archive exceeds the size cap"));
        return out;
    }

    // The end-of-central-directory record lives in the last 22 bytes plus an
    // optional comment of up to 65535 bytes.
    const qint64 tailSize = qMin<qint64>(fileSize, 22 + 65535);
    if (!file.seek(fileSize - tailSize)) {
        out.failure = archiveFail(ArchiveError::Truncated,
                                  QStringLiteral("cannot seek to the archive tail"));
        return out;
    }
    const QByteArray tail = file.read(tailSize);
    if (tail.size() != tailSize) {
        out.failure = archiveFail(ArchiveError::Truncated,
                                  QStringLiteral("short read on the archive tail"));
        return out;
    }

    int eocd = -1;
    for (int i = static_cast<int>(tail.size()) - 22; i >= 0; --i) {
        if (readU32(tail, i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        out.failure = archiveFail(ArchiveError::NotAZipArchive,
                                  QStringLiteral("no end-of-central-directory record"));
        return out;
    }

    const quint16 diskNumber = readU16(tail, eocd + 4);
    const quint16 cdStartDisk = readU16(tail, eocd + 6);
    const quint16 entriesThisDisk = readU16(tail, eocd + 8);
    const quint16 totalEntries = readU16(tail, eocd + 10);
    const quint32 cdSize = readU32(tail, eocd + 12);
    const quint32 cdOffset = readU32(tail, eocd + 16);

    if (diskNumber != 0 || cdStartDisk != 0 || entriesThisDisk != totalEntries) {
        out.failure = archiveFail(ArchiveError::UnsupportedMultiDisk,
                                  QStringLiteral("multi-disk archives are refused"));
        return out;
    }
    if (totalEntries == 0xFFFFu || cdSize == 0xFFFFFFFFu
        || cdOffset == 0xFFFFFFFFu) {
        out.failure = archiveFail(ArchiveError::UnsupportedZip64,
                                  QStringLiteral("ZIP64 archives are refused"));
        return out;
    }
    if (totalEntries > limits.maxEntries) {
        out.failure = archiveFail(ArchiveError::TooManyEntries,
                                  QStringLiteral("archive declares too many entries"));
        return out;
    }
    if (cdSize > 64u * 1024u * 1024u
        || static_cast<qint64>(cdOffset) + static_cast<qint64>(cdSize) > fileSize) {
        out.failure = archiveFail(ArchiveError::Truncated,
                                  QStringLiteral("central directory is out of range"));
        return out;
    }

    if (!file.seek(cdOffset)) {
        out.failure = archiveFail(ArchiveError::Truncated,
                                  QStringLiteral("cannot seek to the central directory"));
        return out;
    }
    const QByteArray cd = file.read(cdSize);
    if (cd.size() != static_cast<qsizetype>(cdSize)) {
        out.failure = archiveFail(ArchiveError::Truncated,
                                  QStringLiteral("short read on the central directory"));
        return out;
    }

    int offset = 0;
    for (quint16 index = 0; index < totalEntries; ++index) {
        if (offset + 46 > cd.size()) {
            out.failure = archiveFail(ArchiveError::Truncated,
                                      QStringLiteral("central directory entry is truncated"));
            return out;
        }
        if (readU32(cd, offset) != 0x02014b50u) {
            out.failure = archiveFail(ArchiveError::Truncated,
                                      QStringLiteral("bad central directory signature"));
            return out;
        }

        CentralEntry entry;
        entry.versionMadeBy = readU16(cd, offset + 4);
        entry.flags = readU16(cd, offset + 8);
        entry.method = readU16(cd, offset + 10);
        entry.crc = readU32(cd, offset + 16);
        entry.compressedSize = readU32(cd, offset + 20);
        entry.uncompressedSize = readU32(cd, offset + 24);
        const quint16 nameLen = readU16(cd, offset + 28);
        const quint16 extraLen = readU16(cd, offset + 30);
        const quint16 commentLen = readU16(cd, offset + 32);
        const quint16 entryDisk = readU16(cd, offset + 34);
        entry.externalAttributes = readU32(cd, offset + 38);
        entry.localHeaderOffset = readU32(cd, offset + 42);

        if (offset + 46 + nameLen + extraLen + commentLen > cd.size()) {
            out.failure = archiveFail(ArchiveError::Truncated,
                                      QStringLiteral("central directory entry overruns"));
            return out;
        }
        if (entryDisk != 0) {
            out.failure = archiveFail(ArchiveError::UnsupportedMultiDisk,
                                      QStringLiteral("entry lives on another disk"));
            return out;
        }
        if (entry.compressedSize == 0xFFFFFFFFu
            || entry.uncompressedSize == 0xFFFFFFFFu
            || entry.localHeaderOffset == 0xFFFFFFFFu) {
            out.failure = archiveFail(ArchiveError::UnsupportedZip64,
                                      QStringLiteral("ZIP64 entry is refused"));
            return out;
        }

        const QByteArray rawName = cd.mid(offset + 46, nameLen);
        // Bit 11 of the general purpose flags declares a UTF-8 name. Anything
        // else is decoded as Latin-1, which round-trips the ASCII names our
        // own packaging produces without inventing a code page.
        entry.name = (entry.flags & 0x0800u) ? QString::fromUtf8(rawName)
                                             : QString::fromLatin1(rawName);
        out.entries.append(entry);
        offset += 46 + nameLen + extraLen + commentLen;
    }

    return out;
}

// Validates one entry's declared metadata (not its path). Returns an ok()
// result when the entry may proceed.
ArchiveResult validateEntryMetadata(const CentralEntry &entry,
                                    const ArchiveLimits &limits,
                                    qint64 totalSoFar)
{
    if ((entry.flags & 0x0001u) != 0 || (entry.flags & 0x0040u) != 0)
        return archiveFail(ArchiveError::UnsupportedEncryption,
                           QStringLiteral("encrypted entries are refused"),
                           entry.name);
    if (entry.isSymlink())
        return archiveFail(ArchiveError::SymlinkEntryRejected,
                           QStringLiteral("symbolic-link entries are refused"),
                           entry.name);
    if (!entry.isRegularOrUnspecified())
        return archiveFail(ArchiveError::SymlinkEntryRejected,
                           QStringLiteral("only regular files and directories are "
                                          "extracted"),
                           entry.name);
    if (entry.method != 0 && entry.method != 8)
        return archiveFail(ArchiveError::UnsupportedCompression,
                           QStringLiteral("only stored and deflated entries are "
                                          "supported"),
                           entry.name);
    if (entry.isDirectory())
        return ArchiveResult();

    if (static_cast<qint64>(entry.uncompressedSize) > limits.maxEntryUncompressedBytes)
        return archiveFail(ArchiveError::EntrySizeExceeded,
                           QStringLiteral("entry exceeds the per-entry size cap"),
                           entry.name);
    if (totalSoFar + static_cast<qint64>(entry.uncompressedSize)
        > limits.maxTotalUncompressedBytes)
        return archiveFail(ArchiveError::TotalSizeExceeded,
                           QStringLiteral("archive exceeds the total size cap"),
                           entry.name);
    if (entry.compressedSize > 0
        && static_cast<qint64>(entry.uncompressedSize)
               > static_cast<qint64>(entry.compressedSize) * limits.maxCompressionRatio)
        return archiveFail(ArchiveError::CompressionRatioExceeded,
                           QStringLiteral("entry expands beyond the allowed ratio"),
                           entry.name);
    if (entry.method == 8 && !deflateSupportAvailable())
        return archiveFail(ArchiveError::InflateUnavailable,
                           QStringLiteral("this build cannot inflate deflated "
                                          "entries"),
                           entry.name);
    return ArchiveResult();
}

// Positions `file` at the start of the entry's compressed payload, using the
// LOCAL header only to learn how many bytes of name/extra to skip. Sizes and
// the CRC always come from the central directory.
bool seekToEntryData(QFile &file, const CentralEntry &entry, QString *error)
{
    if (!file.seek(entry.localHeaderOffset)) {
        *error = QStringLiteral("cannot seek to the local header");
        return false;
    }
    const QByteArray header = file.read(30);
    if (header.size() != 30) {
        *error = QStringLiteral("short read on the local header");
        return false;
    }
    if (readU32(header, 0) != 0x04034b50u) {
        *error = QStringLiteral("bad local header signature");
        return false;
    }
    const quint16 nameLen = readU16(header, 26);
    const quint16 extraLen = readU16(header, 28);
    const qint64 dataStart = static_cast<qint64>(entry.localHeaderOffset) + 30
                             + nameLen + extraLen;
    if (dataStart + static_cast<qint64>(entry.compressedSize) > file.size()) {
        *error = QStringLiteral("entry payload runs past the end of the archive");
        return false;
    }
    if (!file.seek(dataStart)) {
        *error = QStringLiteral("cannot seek to the entry payload");
        return false;
    }
    return true;
}

using SinkFn = std::function<bool(const char *, qsizetype)>;

// Streams the entry payload through `sink`, enforcing the caps as it goes and
// verifying the CRC-32 at the end.
ArchiveResult streamEntry(QFile &file, const CentralEntry &entry,
                          const ArchiveLimits &limits, const SinkFn &sink,
                          qint64 *bytesOut)
{
    constexpr qsizetype kChunk = 64 * 1024;
    quint32 crc = 0;
    qint64 produced = 0;

    // A genuinely empty member: nothing to read, nothing to inflate. The CRC
    // of no bytes is 0, and anything else means the entry is lying.
    if (entry.compressedSize == 0 && entry.uncompressedSize == 0) {
        if (entry.crc != 0)
            return archiveFail(ArchiveError::ChecksumMismatch,
                               QStringLiteral("empty entry declares a non-zero CRC"),
                               entry.name);
        return ArchiveResult();
    }

    auto emitBytes = [&](const char *data, qsizetype length) -> ArchiveResult {
        produced += length;
        if (produced > static_cast<qint64>(entry.uncompressedSize))
            return archiveFail(ArchiveError::EntrySizeExceeded,
                               QStringLiteral("entry produced more bytes than it "
                                              "declared"),
                               entry.name);
        if (produced > limits.maxEntryUncompressedBytes)
            return archiveFail(ArchiveError::EntrySizeExceeded,
                               QStringLiteral("entry exceeds the per-entry size cap"),
                               entry.name);
        crc = crc32Update(crc, data, length);
        if (!sink(data, length))
            return archiveFail(ArchiveError::WriteFailed,
                               QStringLiteral("writing the extracted entry failed"),
                               entry.name);
        return ArchiveResult();
    };

    if (entry.method == 0) {
        if (entry.compressedSize != entry.uncompressedSize)
            return archiveFail(ArchiveError::Truncated,
                               QStringLiteral("stored entry has mismatched sizes"),
                               entry.name);
        qint64 remaining = entry.compressedSize;
        QByteArray buffer;
        while (remaining > 0) {
            buffer = file.read(qMin<qint64>(kChunk, remaining));
            if (buffer.isEmpty())
                return archiveFail(ArchiveError::Truncated,
                                   QStringLiteral("short read on a stored entry"),
                                   entry.name);
            const ArchiveResult emitted = emitBytes(buffer.constData(),
                                                    buffer.size());
            if (!emitted.ok())
                return emitted;
            remaining -= buffer.size();
        }
    } else {
#if defined(LIGHTNING_UPDATER_HAVE_ZLIB)
        z_stream zs;
        std::memset(&zs, 0, sizeof(zs));
        // Negative windowBits selects a RAW deflate stream — a ZIP member has
        // no zlib header and no adler32 trailer.
        if (::inflateInit2(&zs, -MAX_WBITS) != Z_OK)
            return archiveFail(ArchiveError::InflateFailed,
                               QStringLiteral("cannot initialise the inflater"),
                               entry.name);

        QByteArray inBuffer;
        QByteArray outBuffer(kChunk, '\0');
        qint64 remaining = entry.compressedSize;
        int status = Z_OK;
        ArchiveResult failure;

        while (failure.ok()) {
            if (zs.avail_in == 0) {
                if (remaining == 0)
                    break;
                inBuffer = file.read(qMin<qint64>(kChunk, remaining));
                if (inBuffer.isEmpty()) {
                    failure = archiveFail(ArchiveError::Truncated,
                                          QStringLiteral("short read on a deflated "
                                                         "entry"),
                                          entry.name);
                    break;
                }
                remaining -= inBuffer.size();
                zs.next_in = reinterpret_cast<Bytef *>(inBuffer.data());
                zs.avail_in = static_cast<uInt>(inBuffer.size());
            }
            zs.next_out = reinterpret_cast<Bytef *>(outBuffer.data());
            zs.avail_out = static_cast<uInt>(outBuffer.size());
            status = ::inflate(&zs, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END
                && status != Z_BUF_ERROR) {
                failure = archiveFail(ArchiveError::InflateFailed,
                                      QStringLiteral("the deflate stream is corrupt"),
                                      entry.name);
                break;
            }
            const qsizetype got = outBuffer.size()
                                  - static_cast<qsizetype>(zs.avail_out);
            if (got > 0) {
                failure = emitBytes(outBuffer.constData(), got);
                if (!failure.ok())
                    break;
            }
            if (status == Z_STREAM_END)
                break;
            if (status == Z_BUF_ERROR && got == 0 && zs.avail_in == 0
                && remaining == 0) {
                failure = archiveFail(ArchiveError::Truncated,
                                      QStringLiteral("the deflate stream ended early"),
                                      entry.name);
                break;
            }
        }
        ::inflateEnd(&zs);
        if (!failure.ok())
            return failure;
        if (status != Z_STREAM_END)
            return archiveFail(ArchiveError::Truncated,
                               QStringLiteral("the deflate stream is incomplete"),
                               entry.name);
#else
        return archiveFail(ArchiveError::InflateUnavailable,
                           QStringLiteral("this build cannot inflate deflated "
                                          "entries"),
                           entry.name);
#endif
    }

    if (produced != static_cast<qint64>(entry.uncompressedSize))
        return archiveFail(ArchiveError::Truncated,
                           QStringLiteral("entry produced fewer bytes than it "
                                          "declared"),
                           entry.name);
    if (crc != entry.crc)
        return archiveFail(ArchiveError::ChecksumMismatch,
                           QStringLiteral("entry CRC-32 does not match"),
                           entry.name);
    if (bytesOut)
        *bytesOut += produced;
    return ArchiveResult();
}

} // namespace

ArchiveResult listZipEntries(const QString &zipPath, const QString &stagingRoot,
                             QStringList *outNames, const ArchiveLimits &limits)
{
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly))
        return archiveFail(ArchiveError::OpenFailed,
                           QStringLiteral("cannot open the archive"));

    CentralDirectory cd = readCentralDirectory(file, limits);
    if (!cd.failure.ok())
        return cd.failure;

    qint64 total = 0;
    for (const CentralEntry &entry : std::as_const(cd.entries)) {
        const ArchivePathVerdict verdict = checkArchivePath(entry.name, stagingRoot);
        if (verdict != ArchivePathVerdict::Safe)
            return archiveFail(ArchiveError::UnsafeEntryPath,
                               QStringLiteral("unsafe entry path (%1)")
                                   .arg(QLatin1String(archivePathVerdictName(verdict))),
                               entry.name);
        const ArchiveResult metadata = validateEntryMetadata(entry, limits, total);
        if (!metadata.ok())
            return metadata;
        if (!entry.isDirectory())
            total += entry.uncompressedSize;
        if (outNames)
            outNames->append(entry.name);
    }

    ArchiveResult result;
    result.entriesWritten = static_cast<int>(cd.entries.size());
    return result;
}

ArchiveResult extractZipSafely(const QString &zipPath, const QString &stagingRoot,
                               const ArchiveLimits &limits)
{
    const QFileInfo rootInfo(stagingRoot);
    if (!rootInfo.isAbsolute() || !rootInfo.exists() || !rootInfo.isDir())
        return archiveFail(ArchiveError::DestinationUnusable,
                           QStringLiteral("staging root must be an existing "
                                          "absolute directory"));
    {
        const QDir rootDir(stagingRoot);
        if (!rootDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                               | QDir::Hidden | QDir::System)
                 .isEmpty())
            return archiveFail(ArchiveError::DestinationNotEmpty,
                               QStringLiteral("staging root is not empty"));
    }

    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly))
        return archiveFail(ArchiveError::OpenFailed,
                           QStringLiteral("cannot open the archive"));

    CentralDirectory cd = readCentralDirectory(file, limits);
    if (!cd.failure.ok())
        return cd.failure;
    if (cd.entries.size() > limits.maxEntries)
        return archiveFail(ArchiveError::TooManyEntries,
                           QStringLiteral("archive contains too many entries"));

    ArchiveResult result;
    qint64 total = 0;

    for (const CentralEntry &entry : std::as_const(cd.entries)) {
        const ArchivePathVerdict verdict = checkArchivePath(entry.name, stagingRoot);
        if (verdict != ArchivePathVerdict::Safe)
            return archiveFail(ArchiveError::UnsafeEntryPath,
                               QStringLiteral("unsafe entry path (%1)")
                                   .arg(QLatin1String(archivePathVerdictName(verdict))),
                               entry.name);

        const ArchiveResult metadata = validateEntryMetadata(entry, limits, total);
        if (!metadata.ok())
            return metadata;

        const QString destination = resolvedArchiveDestination(entry.name,
                                                               stagingRoot);
        if (destination.isEmpty())
            return archiveFail(ArchiveError::UnsafeEntryPath,
                               QStringLiteral("entry path could not be resolved"),
                               entry.name);

        if (entry.isDirectory()) {
            if (!QDir().mkpath(destination))
                return archiveFail(ArchiveError::WriteFailed,
                                   QStringLiteral("cannot create a directory"),
                                   entry.name);
            if (pathTraversesSymlink(stagingRoot, destination))
                return archiveFail(ArchiveError::SymlinkEntryRejected,
                                   QStringLiteral("destination traverses a symbolic "
                                                  "link"),
                                   entry.name);
            continue;
        }

        const QString parent = QFileInfo(destination).absolutePath();
        if (!QDir().mkpath(parent))
            return archiveFail(ArchiveError::WriteFailed,
                               QStringLiteral("cannot create the parent directory"),
                               entry.name);
        // Even though symlink ENTRIES are refused, re-check the realised path:
        // it is the only way to be sure nothing already present in the staging
        // tree redirects the write.
        if (pathTraversesSymlink(stagingRoot, destination))
            return archiveFail(ArchiveError::SymlinkEntryRejected,
                               QStringLiteral("destination traverses a symbolic link"),
                               entry.name);
        if (QFileInfo::exists(destination))
            return archiveFail(ArchiveError::WriteFailed,
                               QStringLiteral("duplicate entry name"), entry.name);

        QString seekError;
        if (!seekToEntryData(file, entry, &seekError))
            return archiveFail(ArchiveError::Truncated, seekError, entry.name);

        QSaveFile out(destination);
        if (!out.open(QIODevice::WriteOnly))
            return archiveFail(ArchiveError::WriteFailed,
                               QStringLiteral("cannot create the output file"),
                               entry.name);

        const SinkFn sink = [&out](const char *data, qsizetype length) {
            return out.write(data, length) == length;
        };
        const ArchiveResult streamed = streamEntry(file, entry, limits, sink,
                                                   &result.bytesWritten);
        if (!streamed.ok()) {
            out.cancelWriting();
            return streamed;
        }
        if (!out.commit())
            return archiveFail(ArchiveError::WriteFailed,
                               QStringLiteral("cannot commit the output file"),
                               entry.name);

        QFile::Permissions permissions = QFile::ReadOwner | QFile::WriteOwner
                                       | QFile::ReadUser | QFile::WriteUser
                                       | QFile::ReadGroup | QFile::ReadOther;
        if (entry.isExecutable())
            permissions |= QFile::ExeOwner | QFile::ExeUser | QFile::ExeGroup
                         | QFile::ExeOther;
        QFile::setPermissions(destination, permissions);

        total += entry.uncompressedSize;
        ++result.entriesWritten;
    }

    return result;
}

} // namespace updater
