// lightning-updater: archive path safety and the hardened ZIP reader.
//
// This is the highest-risk code in the helper: a single accepted traversal
// entry writes attacker-chosen bytes to an attacker-chosen path with the
// user's privileges. The verdict function is therefore pure and is exercised
// here against an exhaustive hostile matrix, and the reader itself is driven
// with real archives built byte-by-byte in this file.
//
// The path, symlink, cap and CRC refusals are proved with STORED entries, so
// they hold regardless of whether the build has zlib. The deflate half of the
// suite builds REAL method-8 members (raw deflate, no zlib wrapper — exactly
// what a ZIP member carries), because that is what `zip -X` produces and
// therefore what the shipped portable archive actually is: it exercises the
// inflate loop, the streaming per-chunk caps, the compression-ratio guard,
// truncated and corrupt streams, and the CRC-32 taken over INFLATED output.
// Those cases QSKIP — never silently pass — in a build without zlib.

#include "updater/SafeArchive.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstring>
#include <functional>

#if defined(LIGHTNING_UPDATER_HAVE_ZLIB)
#include <zlib.h>
#endif

using namespace updater;

namespace {

#if defined(LIGHTNING_UPDATER_HAVE_ZLIB)
constexpr bool kTestBuildHasZlib = true;
#else
constexpr bool kTestBuildHasZlib = false;
#endif

quint32 crc32Of(const QByteArray &data)
{
    static quint32 table[256];
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    quint32 crc = 0xFFFFFFFFu;
    for (char byte : data)
        crc = table[(crc ^ static_cast<quint8>(byte)) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void appendU16(QByteArray &out, quint16 value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
}

void appendU32(QByteArray &out, quint32 value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 24) & 0xFF));
}

// Raw DEFLATE: no zlib header, no adler32 trailer. A ZIP member carries the
// bare stream, which is why SafeArchive inflates with a negative windowBits —
// so the fixture has to produce it the same way. compress2() would emit a
// zlib-wrapped stream and prove nothing about the real path.
//
// Returns an empty array when this build has no zlib; every deflate case
// skips in that configuration rather than passing vacuously.
QByteArray deflateRaw(const QByteArray &plain)
{
#if defined(LIGHTNING_UPDATER_HAVE_ZLIB)
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (::deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                       Z_DEFAULT_STRATEGY)
        != Z_OK) {
        return QByteArray();
    }
    QByteArray out;
    out.resize(static_cast<qsizetype>(
                   ::deflateBound(&zs, static_cast<uLong>(plain.size())))
               + 64);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
    zs.avail_in = static_cast<uInt>(plain.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    const int status = ::deflate(&zs, Z_FINISH);
    const qsizetype produced = out.size() - static_cast<qsizetype>(zs.avail_out);
    ::deflateEnd(&zs);
    if (status != Z_STREAM_END)
        return QByteArray();
    out.resize(produced);
    return out;
#else
    Q_UNUSED(plain);
    return QByteArray();
#endif
}

// Deterministic, genuinely compressible bytes: one pseudo-random block
// repeated, which is roughly what a real bundle's padding and string tables
// look like to deflate — compressible, but nothing like a zip bomb. The LCG
// keeps the fixture identical on every platform, so the achieved compressed
// size (which two tests derive their limits from) is stable.
QByteArray compressibleBlob(int blockSize, int repeats, quint32 seed)
{
    QByteArray block(blockSize, '\0');
    quint32 state = seed ? seed : 1u;
    for (int i = 0; i < blockSize; ++i) {
        state = state * 1664525u + 1013904223u;
        block[i] = static_cast<char>((state >> 24) & 0xFFu);
    }
    QByteArray out;
    out.reserve(static_cast<qsizetype>(blockSize) * repeats);
    for (int i = 0; i < repeats; ++i)
        out.append(block);
    return out;
}

struct ZipEntry {
    QByteArray name;
    QByteArray contents;
    quint16 versionMadeBy = 0x0300;   // Unix
    quint32 externalAttributes = 0;   // high 16 bits = unix mode
    bool corruptCrc = false;
    // 0 = stored, 8 = deflate. Anything else is rejected by the writer: the
    // fixture only builds what a real portable ZIP contains.
    quint16 method = 0;
    // The size the headers DECLARE. -1 means the truthful one; a smaller
    // value is how a hostile archive claims to be smaller than it inflates to.
    qint64 declaredUncompressedSize = -1;
    // Applied to the compressed payload after compression and before the
    // sizes are taken, so truncation and corruption stay self-consistent.
    std::function<void(QByteArray &)> mutatePayload;
};

ZipEntry regularFile(const char *name, const QByteArray &contents,
                     quint16 unixMode = 0644)
{
    ZipEntry entry;
    entry.name = QByteArray(name);
    entry.contents = contents;
    entry.externalAttributes =
        (static_cast<quint32>(0x8000u | unixMode) << 16);
    return entry;
}

// The same regular file, carried as a real method-8 member.
ZipEntry deflatedFile(const char *name, const QByteArray &contents,
                      quint16 unixMode = 0644)
{
    ZipEntry entry = regularFile(name, contents, unixMode);
    entry.method = 8;
    return entry;
}

ZipEntry symlinkEntry(const char *name, const QByteArray &target)
{
    ZipEntry entry;
    entry.name = QByteArray(name);
    entry.contents = target;
    entry.externalAttributes = (static_cast<quint32>(0xA1FFu) << 16);
    return entry;
}

// Writes a real, minimal, valid ZIP. Members are stored (method 0) unless the
// entry asks for deflate (method 8), in which case the payload is a genuine
// raw deflate stream. The CRC always covers the ORIGINAL bytes, which is what
// makes the truncation case honest: the archive declares the checksum the
// complete entry would have had.
bool writeZip(const QString &path, const QList<ZipEntry> &entries)
{
    QByteArray body;
    QByteArray central;
    quint16 count = 0;

    for (const ZipEntry &entry : entries) {
        const quint32 offset = static_cast<quint32>(body.size());
        quint32 crc = crc32Of(entry.contents);
        if (entry.corruptCrc)
            crc ^= 0xDEADBEEFu;

        QByteArray payload;
        if (entry.method == 0) {
            payload = entry.contents;
        } else if (entry.method == 8) {
            payload = deflateRaw(entry.contents);
            if (payload.isEmpty())
                return false; // no zlib in this build, or deflate failed
        } else {
            return false;
        }
        if (entry.mutatePayload)
            entry.mutatePayload(payload);

        const quint32 compressed = static_cast<quint32>(payload.size());
        const quint32 size = static_cast<quint32>(
            entry.declaredUncompressedSize >= 0 ? entry.declaredUncompressedSize
                                                : entry.contents.size());

        // Bit 11 declares a UTF-8 entry name, which is what real packaging
        // tools set and what makes the non-ASCII cases below meaningful.
        constexpr quint16 kUtf8NameFlag = 0x0800;

        body.append("PK\x03\x04", 4);
        appendU16(body, 20);   // version needed
        appendU16(body, kUtf8NameFlag);
        appendU16(body, entry.method);
        appendU16(body, 0);    // mod time
        appendU16(body, 0);    // mod date
        appendU32(body, crc);
        appendU32(body, compressed);
        appendU32(body, size); // uncompressed
        appendU16(body, static_cast<quint16>(entry.name.size()));
        appendU16(body, 0);    // extra length
        body.append(entry.name);
        body.append(payload);

        central.append("PK\x01\x02", 4);
        appendU16(central, entry.versionMadeBy);
        appendU16(central, 20);            // version needed
        appendU16(central, kUtf8NameFlag); // flags
        appendU16(central, entry.method);
        appendU16(central, 0);             // mod time
        appendU16(central, 0);             // mod date
        appendU32(central, crc);
        appendU32(central, compressed);
        appendU32(central, size);
        appendU16(central, static_cast<quint16>(entry.name.size()));
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0);  // disk number start
        appendU16(central, 0);  // internal attributes
        appendU32(central, entry.externalAttributes);
        appendU32(central, offset);
        central.append(entry.name);
        ++count;
    }

    QByteArray archive = body;
    const quint32 centralOffset = static_cast<quint32>(archive.size());
    archive.append(central);
    archive.append("PK\x05\x06", 4);
    appendU16(archive, 0);
    appendU16(archive, 0);
    appendU16(archive, count);
    appendU16(archive, count);
    appendU32(archive, static_cast<quint32>(central.size()));
    appendU32(archive, centralOffset);
    appendU16(archive, 0);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(archive) == archive.size();
}

} // namespace

class UpdaterZipSafetyTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // --- the pure verdict function ---
    void hostileEntryNamesAreRejected_data();
    void hostileEntryNamesAreRejected();
    void benignEntryNamesAreAccepted_data();
    void benignEntryNamesAreAccepted();
    void rootMustBeAbsolute();
    void symlinkedStagingRootStillContains();
    void resolvedDestinationIsEmptyForUnsafeNames();
    void pathTraversingASymlinkIsDetected();

    // --- the reader ---
    void extractsAStoredArchive();
    void preservesTheExecutableBit();
    void refusesATraversalEntry();
    void refusesAnAbsoluteEntry();
    void refusesADriveLetterEntry();
    void refusesASymlinkEntry();
    void refusesACorruptCrc();
    void refusesANonZipFile();
    void refusesATruncatedArchive();
    void refusesANonEmptyStagingRoot();
    void enforcesTheEntryCountCap();
    void enforcesTheTotalSizeCap();
    void enforcesThePerEntrySizeCap();
    void listingAppliesTheSameVerdicts();

    // --- the reader, deflated (method 8) entries ---
    void extractsADeflatedArchive();
    void extractsAMixedStoredAndDeflatedArchive();
    void extractsADeflatedEmptyEntry();
    void refusesDeflateOutputBeyondTheDeclaredSize();
    void refusesAnInvalidDeflateBlockType();
    void refusesACorruptDeflateStream();
    void refusesATruncatedDeflateStream();
    void refusesABadCrcOverInflatedOutput();
    void refusesADeflateBombAtTheDefaultRatioCap();
    void acceptsAnEntryAtTheCompressionRatioCap();
    void refusesADeflatedTraversalEntry();
    void refusesADeflatedSymlinkEntry();

private:
    // Every file anywhere under the staging root. A refusal must leave none
    // of them behind: neither the entry itself nor a QSaveFile temporary.
    QStringList filesUnderStaging() const;
    void resetStaging();

    QTemporaryDir m_dir;
    QString m_staging;
    QString m_zip;
};

void UpdaterZipSafetyTest::init()
{
    QVERIFY(m_dir.isValid());
    m_staging = QDir(m_dir.path()).absoluteFilePath(QStringLiteral("staging"));
    QDir(m_staging).removeRecursively();
    QVERIFY(QDir().mkpath(m_staging));
    m_zip = QDir(m_dir.path()).absoluteFilePath(QStringLiteral("portable.zip"));
    QFile::remove(m_zip);
}

void UpdaterZipSafetyTest::cleanup()
{
    QDir(m_staging).removeRecursively();
    QFile::remove(m_zip);
}

QStringList UpdaterZipSafetyTest::filesUnderStaging() const
{
    QStringList found;
    QDirIterator it(m_staging,
                    QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        found.append(it.next());
    return found;
}

void UpdaterZipSafetyTest::resetStaging()
{
    QVERIFY(QDir(m_staging).removeRecursively());
    QVERIFY(QDir().mkpath(m_staging));
}

// ---------------------------------------------------------------------------
// The pure verdict function
// ---------------------------------------------------------------------------

void UpdaterZipSafetyTest::hostileEntryNamesAreRejected_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<int>("verdict");

    const auto row = [](const char *tag, const QString &name,
                        ArchivePathVerdict verdict) {
        QTest::newRow(tag) << name << int(verdict);
    };

    row("empty", QString(), ArchivePathVerdict::EmptyName);
    row("nul-leading", QString(QChar(0)) + QStringLiteral("a.txt"),
        ArchivePathVerdict::EmbeddedNul);
    row("nul-embedded", QStringLiteral("a") + QString(QChar(0)) + QStringLiteral("b"),
        ArchivePathVerdict::EmbeddedNul);
    row("nul-trailing", QStringLiteral("a.txt") + QString(QChar(0)),
        ArchivePathVerdict::EmbeddedNul);
    row("newline", QStringLiteral("a\nb"), ArchivePathVerdict::ControlCharacter);
    row("delete-char", QStringLiteral("a") + QString(QChar(0x7F)),
        ArchivePathVerdict::ControlCharacter);
    row("too-long", QString(600, QLatin1Char('a')), ArchivePathVerdict::TooLong);

    row("posix-absolute", QStringLiteral("/etc/passwd"),
        ArchivePathVerdict::AbsolutePosix);
    row("posix-absolute-root", QStringLiteral("/"),
        ArchivePathVerdict::AbsolutePosix);
    row("drive-backslash", QStringLiteral("C:\\Windows\\System32\\x.dll"),
        ArchivePathVerdict::AbsoluteDriveLetter);
    row("drive-forward", QStringLiteral("c:/Windows/x.dll"),
        ArchivePathVerdict::AbsoluteDriveLetter);
    row("drive-relative", QStringLiteral("C:x.dll"),
        ArchivePathVerdict::AbsoluteDriveLetter);
    row("unc-backslash", QStringLiteral("\\\\server\\share\\x"),
        ArchivePathVerdict::UncPath);
    row("unc-forward", QStringLiteral("//server/share/x"),
        ArchivePathVerdict::UncPath);
    row("leading-backslash", QStringLiteral("\\Windows\\x"),
        ArchivePathVerdict::BackslashSeparator);
    row("backslash-traversal", QStringLiteral("a\\..\\..\\x"),
        ArchivePathVerdict::BackslashSeparator);
    row("backslash-inner", QStringLiteral("dir\\file.txt"),
        ArchivePathVerdict::BackslashSeparator);

    row("parent-simple", QStringLiteral("../x"),
        ArchivePathVerdict::ParentTraversal);
    row("parent-nested", QStringLiteral("a/../../x"),
        ArchivePathVerdict::ParentTraversal);
    row("parent-deep", QStringLiteral("a/b/c/../../../../x"),
        ArchivePathVerdict::ParentTraversal);
    row("parent-only", QStringLiteral(".."),
        ArchivePathVerdict::ParentTraversal);
    row("parent-trailing", QStringLiteral("a/.."),
        ArchivePathVerdict::ParentTraversal);
    row("current-dir", QStringLiteral("./a"),
        ArchivePathVerdict::CurrentDirComponent);
    row("current-dir-inner", QStringLiteral("a/./b"),
        ArchivePathVerdict::CurrentDirComponent);
    row("double-slash", QStringLiteral("a//b"),
        ArchivePathVerdict::EmptyComponent);

    row("reserved-con", QStringLiteral("CON"),
        ArchivePathVerdict::ReservedDeviceName);
    row("reserved-nul-name", QStringLiteral("dir/nul.txt"),
        ArchivePathVerdict::ReservedDeviceName);
    row("reserved-com1", QStringLiteral("COM1.dll"),
        ArchivePathVerdict::ReservedDeviceName);
    row("reserved-lpt9", QStringLiteral("a/lpt9"),
        ArchivePathVerdict::ReservedDeviceName);
    row("trailing-dot", QStringLiteral("a/b."),
        ArchivePathVerdict::TrailingDotOrSpace);
    row("trailing-space", QStringLiteral("a/b "),
        ArchivePathVerdict::TrailingDotOrSpace);

    QString deep;
    for (int i = 0; i < 40; ++i)
        deep += QStringLiteral("d/");
    deep += QStringLiteral("file.txt");
    row("too-deep", deep, ArchivePathVerdict::TooLong);
}

void UpdaterZipSafetyTest::hostileEntryNamesAreRejected()
{
    QFETCH(QString, name);
    QFETCH(int, verdict);

    const ArchivePathVerdict actual = checkArchivePath(name, m_staging);
    QCOMPARE(int(actual), verdict);
    QVERIFY(!isSafeArchivePath(name, m_staging));
    // Whatever the specific verdict, the destination must never be produced.
    QVERIFY(resolvedArchiveDestination(name, m_staging).isEmpty());
}

void UpdaterZipSafetyTest::benignEntryNamesAreAccepted_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("plain") << QStringLiteral("matrix-client.exe");
    QTest::newRow("nested") << QStringLiteral("plugins/platforms/qwindows.dll");
    QTest::newRow("directory") << QStringLiteral("plugins/");
    QTest::newRow("dotfile") << QStringLiteral(".keep");
    QTest::newRow("spaces") << QStringLiteral("Lightning Files/read me.txt");
    QTest::newRow("unicode") << QStringLiteral("ištekliai/ąčęėįšųūž.txt");
    QTest::newRow("parentheses") << QStringLiteral("data/icon (large).png");
    QTest::newRow("dots-inside") << QStringLiteral("lib/Qt6Core.so.6.5.3");
    QTest::newRow("double-dot-inside-name") << QStringLiteral("a..b/c..d.txt");
    QTest::newRow("com0-not-reserved") << QStringLiteral("COM0.txt");
}

void UpdaterZipSafetyTest::benignEntryNamesAreAccepted()
{
    QFETCH(QString, name);
    QCOMPARE(int(checkArchivePath(name, m_staging)),
             int(ArchivePathVerdict::Safe));
    QVERIFY(isSafeArchivePath(name, m_staging));

    const QString destination = resolvedArchiveDestination(name, m_staging);
    QVERIFY(!destination.isEmpty());
    const QString root = QDir::cleanPath(QFileInfo(m_staging).canonicalFilePath());
    QVERIFY(destination.startsWith(root + QLatin1Char('/')));
}

void UpdaterZipSafetyTest::rootMustBeAbsolute()
{
    QCOMPARE(int(checkArchivePath(QStringLiteral("a.txt"), QString())),
             int(ArchivePathVerdict::InvalidRoot));
    QCOMPARE(int(checkArchivePath(QStringLiteral("a.txt"),
                                  QStringLiteral("relative/dir"))),
             int(ArchivePathVerdict::InvalidRoot));
}

void UpdaterZipSafetyTest::symlinkedStagingRootStillContains()
{
#ifdef Q_OS_WIN
    QSKIP("symlink creation requires elevation on Windows");
#else
    // A staging root reached through a symlink must still contain its
    // entries: the containment check canonicalises the ROOT, so the prefix
    // comparison is made against real paths on both sides.
    const QString link = QDir(m_dir.path()).absoluteFilePath(QStringLiteral("link"));
    QFile::remove(link);
    QVERIFY(QFile::link(m_staging, link));

    QCOMPARE(int(checkArchivePath(QStringLiteral("a/b.txt"), link)),
             int(ArchivePathVerdict::Safe));
    const QString destination =
        resolvedArchiveDestination(QStringLiteral("a/b.txt"), link);
    QVERIFY(destination.startsWith(QFileInfo(m_staging).canonicalFilePath()));
    QFile::remove(link);
#endif
}

void UpdaterZipSafetyTest::resolvedDestinationIsEmptyForUnsafeNames()
{
    QVERIFY(resolvedArchiveDestination(QStringLiteral("../x"), m_staging).isEmpty());
    QVERIFY(resolvedArchiveDestination(QStringLiteral("/x"), m_staging).isEmpty());
    QVERIFY(resolvedArchiveDestination(QStringLiteral("a.txt"), QString()).isEmpty());
}

void UpdaterZipSafetyTest::pathTraversingASymlinkIsDetected()
{
#ifdef Q_OS_WIN
    QSKIP("symlink creation requires elevation on Windows");
#else
    const QDir root(m_staging);
    QVERIFY(QDir().mkpath(root.absoluteFilePath(QStringLiteral("real"))));
    const QString link = root.absoluteFilePath(QStringLiteral("linkdir"));
    QVERIFY(QFile::link(root.absoluteFilePath(QStringLiteral("real")), link));

    QVERIFY(pathTraversesSymlink(m_staging,
                                 root.absoluteFilePath(QStringLiteral("linkdir/x"))));
    QVERIFY(!pathTraversesSymlink(m_staging,
                                  root.absoluteFilePath(QStringLiteral("real/x"))));
    // Anything outside the root is unsafe by definition.
    QVERIFY(pathTraversesSymlink(m_staging, QStringLiteral("/etc/passwd")));
#endif
}

// ---------------------------------------------------------------------------
// The reader
// ---------------------------------------------------------------------------

void UpdaterZipSafetyTest::extractsAStoredArchive()
{
    QVERIFY(writeZip(m_zip, {
        regularFile("matrix-client.exe", QByteArray("MZ binary payload")),
        regularFile("plugins/platforms/qwindows.dll", QByteArray("dll bytes")),
        regularFile("ištekliai/ąčę (1).txt", QString::fromUtf8("labas").toUtf8()),
    }));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY2(result.ok(), archiveErrorName(result.error));
    QCOMPARE(result.entriesWritten, 3);

    const QDir root(m_staging);
    QFile exe(root.absoluteFilePath(QStringLiteral("matrix-client.exe")));
    QVERIFY(exe.open(QIODevice::ReadOnly));
    QCOMPARE(exe.readAll(), QByteArray("MZ binary payload"));

    QVERIFY(QFileInfo::exists(
        root.absoluteFilePath(QStringLiteral("plugins/platforms/qwindows.dll"))));
    QVERIFY(QFileInfo::exists(
        root.absoluteFilePath(QStringLiteral("ištekliai/ąčę (1).txt"))));
}

void UpdaterZipSafetyTest::preservesTheExecutableBit()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX permission bits are not meaningful here");
#else
    QVERIFY(writeZip(m_zip, {
        regularFile("run.sh", QByteArray("#!/bin/sh\n"), 0755),
        regularFile("data.bin", QByteArray("bytes"), 0644),
    }));
    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY2(result.ok(), archiveErrorName(result.error));

    const QDir root(m_staging);
    QVERIFY(QFileInfo(root.absoluteFilePath(QStringLiteral("run.sh")))
                .isExecutable());
    QVERIFY(!QFileInfo(root.absoluteFilePath(QStringLiteral("data.bin")))
                 .isExecutable());
#endif
}

void UpdaterZipSafetyTest::refusesATraversalEntry()
{
    QVERIFY(writeZip(m_zip, {
        regularFile("matrix-client.exe", QByteArray("ok")),
        regularFile("../pwned.txt", QByteArray("owned")),
    }));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::UnsafeEntryPath);
    // The entry would have landed one level above the staging root.
    QVERIFY(!QFileInfo::exists(
        QDir(m_dir.path()).absoluteFilePath(QStringLiteral("pwned.txt"))));
}

void UpdaterZipSafetyTest::refusesAnAbsoluteEntry()
{
    QVERIFY(writeZip(m_zip, {regularFile("/etc/lightning-pwned", QByteArray("x"))}));
    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::UnsafeEntryPath);
}

void UpdaterZipSafetyTest::refusesADriveLetterEntry()
{
    QVERIFY(writeZip(m_zip,
                     {regularFile("C:\\Windows\\System32\\evil.dll", QByteArray("x"))}));
    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::UnsafeEntryPath);
}

void UpdaterZipSafetyTest::refusesASymlinkEntry()
{
    // The name is perfectly safe; only the unix mode marks it a symlink, and
    // its contents would be the link target. Following it would let an
    // archive point anywhere on the filesystem.
    QVERIFY(writeZip(m_zip, {
        regularFile("matrix-client.exe", QByteArray("ok")),
        symlinkEntry("config", QByteArray("/etc/shadow")),
    }));
    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::SymlinkEntryRejected);
    QVERIFY(!QFileInfo(QDir(m_staging).absoluteFilePath(QStringLiteral("config")))
                 .isSymLink());
}

void UpdaterZipSafetyTest::refusesACorruptCrc()
{
    ZipEntry entry = regularFile("matrix-client.exe", QByteArray("payload"));
    entry.corruptCrc = true;
    QVERIFY(writeZip(m_zip, {entry}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::ChecksumMismatch);
}

void UpdaterZipSafetyTest::refusesANonZipFile()
{
    QFile file(m_zip);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(4096, 'A'));
    file.close();

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::NotAZipArchive);
}

void UpdaterZipSafetyTest::refusesATruncatedArchive()
{
    QVERIFY(writeZip(m_zip, {regularFile("a.txt", QByteArray(512, 'z'))}));
    QFile file(m_zip);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.resize(file.size() - 8)); // clip the end-of-central-directory
    file.close();

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QVERIFY(result.error == ArchiveError::NotAZipArchive
            || result.error == ArchiveError::Truncated);
}

void UpdaterZipSafetyTest::refusesANonEmptyStagingRoot()
{
    QVERIFY(writeZip(m_zip, {regularFile("a.txt", QByteArray("x"))}));
    QFile squatter(QDir(m_staging).absoluteFilePath(QStringLiteral("existing")));
    QVERIFY(squatter.open(QIODevice::WriteOnly));
    squatter.close();

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::DestinationNotEmpty);
}

void UpdaterZipSafetyTest::enforcesTheEntryCountCap()
{
    QList<ZipEntry> entries;
    for (int i = 0; i < 6; ++i) {
        entries.append(regularFile(qPrintable(QStringLiteral("f%1.txt").arg(i)),
                                   QByteArray("x")));
    }
    QVERIFY(writeZip(m_zip, entries));

    ArchiveLimits limits;
    limits.maxEntries = 3;
    const ArchiveResult result = extractZipSafely(m_zip, m_staging, limits);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::TooManyEntries);
}

void UpdaterZipSafetyTest::enforcesTheTotalSizeCap()
{
    QVERIFY(writeZip(m_zip, {
        regularFile("a.bin", QByteArray(4096, 'a')),
        regularFile("b.bin", QByteArray(4096, 'b')),
        regularFile("c.bin", QByteArray(4096, 'c')),
    }));

    ArchiveLimits limits;
    limits.maxTotalUncompressedBytes = 8192;
    const ArchiveResult result = extractZipSafely(m_zip, m_staging, limits);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::TotalSizeExceeded);
}

void UpdaterZipSafetyTest::enforcesThePerEntrySizeCap()
{
    QVERIFY(writeZip(m_zip, {regularFile("big.bin", QByteArray(8192, 'x'))}));

    ArchiveLimits limits;
    limits.maxEntryUncompressedBytes = 4096;
    const ArchiveResult result = extractZipSafely(m_zip, m_staging, limits);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::EntrySizeExceeded);
}

void UpdaterZipSafetyTest::listingAppliesTheSameVerdicts()
{
    QVERIFY(writeZip(m_zip, {
        regularFile("matrix-client.exe", QByteArray("ok")),
        regularFile("data/x.txt", QByteArray("ok")),
    }));
    QStringList names;
    ArchiveResult listed = listZipEntries(m_zip, m_staging, &names);
    QVERIFY2(listed.ok(), archiveErrorName(listed.error));
    QCOMPARE(names, QStringList({QStringLiteral("matrix-client.exe"),
                                 QStringLiteral("data/x.txt")}));
    // Listing writes nothing.
    QVERIFY(QDir(m_staging).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());

    QFile::remove(m_zip);
    QVERIFY(writeZip(m_zip, {regularFile("../escape.txt", QByteArray("x"))}));
    names.clear();
    listed = listZipEntries(m_zip, m_staging, &names);
    QVERIFY(!listed.ok());
    QCOMPARE(listed.error, ArchiveError::UnsafeEntryPath);
}

// ---------------------------------------------------------------------------
// The reader, deflated (method 8) entries
//
// This is the shape the shipped portable ZIP actually has: `zip -X` deflates.
// Everything below therefore drives the inflate loop rather than the stored
// fast path — including the two guards that only exist there (the per-chunk
// output cap and the compression-ratio refusal) and the CRC-32, which on this
// path is taken over INFLATED bytes, not over what the archive supplied.
// ---------------------------------------------------------------------------

// Skips, rather than passing vacuously, when either this test binary or
// SafeArchive.cpp itself was built without zlib. Both must agree: the test
// compresses the fixture, the library inflates it.
#define SKIP_WITHOUT_DEFLATE()                                                 \
    do {                                                                       \
        if (!kTestBuildHasZlib || !deflateSupportAvailable())                  \
            QSKIP("built without zlib: deflated entries cannot be exercised"); \
    } while (false)

void UpdaterZipSafetyTest::extractsADeflatedArchive()
{
    SKIP_WITHOUT_DEFLATE();

    // Realistic sizes: each payload is several inflate chunks long (the reader
    // works in 64 KiB steps), so the streaming loop really iterates.
    const QByteArray exe = compressibleBlob(4096, 32, 0x5EEDu);        // 128 KiB
    const QByteArray dll = compressibleBlob(1024, 200, 0xBEEFu);       // 200 KiB
    QByteArray text;                                                   // ~90 KiB
    for (int line = 0; line < 2000; ++line) {
        text += QStringLiteral("ąčę %1 Lightning portable build line\n")
                    .arg(line, 8, 10, QLatin1Char('0'))
                    .toUtf8();
    }

    // Every payload here must be ORDINARY content, not a bomb: a fixture that
    // happened to exceed maxCompressionRatio would be refused and this test
    // would then be asserting nothing about the inflate path. (An earlier
    // draft used one repeated line, which deflate crushed 263:1.)
    const auto isOrdinarilyCompressible = [](const QByteArray &plain) {
        const QByteArray compressed = deflateRaw(plain);
        return !compressed.isEmpty()
            && qint64(plain.size()) <= qint64(compressed.size())
                                           * ArchiveLimits().maxCompressionRatio;
    };
    QVERIFY(isOrdinarilyCompressible(exe));
    QVERIFY(isOrdinarilyCompressible(dll));
    QVERIFY(isOrdinarilyCompressible(text));

    QVERIFY(writeZip(m_zip, {
        deflatedFile("matrix-client.exe", exe),
        deflatedFile("plugins/platforms/qwindows.dll", dll),
        deflatedFile("share/ištekliai/readme.txt", text),
    }));

    // The fixture must actually be compressed, or this proves nothing about
    // the inflate path.
    QVERIFY(QFileInfo(m_zip).size()
            < exe.size() + dll.size() + text.size());

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY2(result.ok(), archiveErrorName(result.error));
    QCOMPARE(result.entriesWritten, 3);
    QCOMPARE(result.bytesWritten,
             qint64(exe.size() + dll.size() + text.size()));

    const QDir root(m_staging);
    const auto readBack = [&root](const QString &relative) {
        QFile file(root.absoluteFilePath(relative));
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    };
    QCOMPARE(readBack(QStringLiteral("matrix-client.exe")), exe);
    QCOMPARE(readBack(QStringLiteral("plugins/platforms/qwindows.dll")), dll);
    QCOMPARE(readBack(QStringLiteral("share/ištekliai/readme.txt")), text);

    // Nesting was created by the reader, not by the fixture.
    QVERIFY(QFileInfo(root.absoluteFilePath(QStringLiteral("plugins/platforms")))
                .isDir());
    QCOMPARE(filesUnderStaging().size(), qsizetype(3));
}

void UpdaterZipSafetyTest::extractsAMixedStoredAndDeflatedArchive()
{
    SKIP_WITHOUT_DEFLATE();

    // A real `zip -X` archive stores what does not shrink and deflates what
    // does, so both members must work in one pass — and the reader must pick
    // the branch per entry, from the central directory's method field.
    const QByteArray stored = QByteArray("incompressible-enough\n");
    const QByteArray deflated = compressibleBlob(2048, 64, 0xA11CEu); // 128 KiB

    QVERIFY(writeZip(m_zip, {
        regularFile("stored.txt", stored),
        deflatedFile("data/deflated.bin", deflated),
        regularFile("also-stored.bin", QByteArray(1024, '\x7e')),
    }));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY2(result.ok(), archiveErrorName(result.error));
    QCOMPARE(result.entriesWritten, 3);

    const QDir root(m_staging);
    QFile a(root.absoluteFilePath(QStringLiteral("stored.txt")));
    QVERIFY(a.open(QIODevice::ReadOnly));
    QCOMPARE(a.readAll(), stored);

    QFile b(root.absoluteFilePath(QStringLiteral("data/deflated.bin")));
    QVERIFY(b.open(QIODevice::ReadOnly));
    QCOMPARE(b.readAll(), deflated);

    QFile c(root.absoluteFilePath(QStringLiteral("also-stored.bin")));
    QVERIFY(c.open(QIODevice::ReadOnly));
    QCOMPARE(c.readAll(), QByteArray(1024, '\x7e'));
}

void UpdaterZipSafetyTest::extractsADeflatedEmptyEntry()
{
    SKIP_WITHOUT_DEFLATE();

    // An empty member is still a real deflate stream (one empty final block),
    // so compressedSize is non-zero while uncompressedSize is 0. The reader
    // must inflate it, produce nothing, and match the CRC of no bytes.
    QVERIFY(writeZip(m_zip, {deflatedFile("empty.txt", QByteArray())}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY2(result.ok(), archiveErrorName(result.error));
    QCOMPARE(result.entriesWritten, 1);
    QCOMPARE(result.bytesWritten, qint64(0));

    const QFileInfo info(QDir(m_staging).absoluteFilePath(QStringLiteral("empty.txt")));
    QVERIFY(info.exists());
    QCOMPARE(info.size(), qint64(0));
}

void UpdaterZipSafetyTest::refusesDeflateOutputBeyondTheDeclaredSize()
{
    SKIP_WITHOUT_DEFLATE();

    // THE bypass of the compression-ratio guard: that guard only ever sees the
    // DECLARED sizes, so an archive that understates uncompressedSize sails
    // past it. The streaming per-chunk cap is what actually stops the write,
    // mid-stream, before the declared budget is exceeded.
    const QByteArray real = compressibleBlob(4096, 64, 0xD0D0u); // 256 KiB
    const qint64 lie = 96 * 1024;                                // < one quarter

    ZipEntry entry = deflatedFile("payload.bin", real);
    entry.declaredUncompressedSize = lie;
    QVERIFY(writeZip(m_zip, {entry}));

    // The lie is small enough that the declared ratio is unremarkable, which
    // is the point: metadata validation cannot catch this one.
    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::EntrySizeExceeded);
    QVERIFY2(filesUnderStaging().isEmpty(),
             qPrintable(filesUnderStaging().join(QLatin1Char(' '))));
    QCOMPARE(result.entriesWritten, 0);
}

void UpdaterZipSafetyTest::refusesAnInvalidDeflateBlockType()
{
    SKIP_WITHOUT_DEFLATE();

    // BTYPE 3 is reserved and can never appear in a valid stream, so this
    // pins the typed error exactly: inflate must fail, not merely disagree
    // about the length or the checksum.
    ZipEntry entry = deflatedFile("payload.bin", compressibleBlob(1024, 32, 1u));
    entry.mutatePayload = [](QByteArray &payload) {
        if (payload.isEmpty())
            return;
        // bit 0 = BFINAL, bits 1-2 = BTYPE; 0b11 is the reserved value.
        payload[0] = static_cast<char>((static_cast<quint8>(payload.at(0)) & 0xF8u)
                                       | 0x06u);
    };
    QVERIFY(writeZip(m_zip, {entry}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::InflateFailed);
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::refusesACorruptDeflateStream()
{
    SKIP_WITHOUT_DEFLATE();

    // Bit rot in the middle of the payload. Which refusal it lands on depends
    // on where the damage falls — a broken Huffman code, a stream that ends
    // early, or output that no longer matches the CRC — so the assertion is
    // that it is refused with one of those typed errors and nothing is left
    // on disk, never that it is one specific code.
    ZipEntry entry = deflatedFile("payload.bin", compressibleBlob(4096, 32, 0x1234u));
    entry.mutatePayload = [](QByteArray &payload) {
        const qsizetype start = payload.size() / 2;
        for (qsizetype i = start; i < qMin(start + 32, payload.size()); ++i)
            payload[i] = static_cast<char>(static_cast<quint8>(payload.at(i)) ^ 0xFFu);
    };
    QVERIFY(writeZip(m_zip, {entry}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY2(!result.ok(), "a corrupt deflate stream was accepted");
    QVERIFY2(result.error == ArchiveError::InflateFailed
                 || result.error == ArchiveError::Truncated
                 || result.error == ArchiveError::ChecksumMismatch
                 || result.error == ArchiveError::EntrySizeExceeded,
             archiveErrorName(result.error));
    QVERIFY2(filesUnderStaging().isEmpty(),
             qPrintable(filesUnderStaging().join(QLatin1Char(' '))));
}

void UpdaterZipSafetyTest::refusesATruncatedDeflateStream()
{
    SKIP_WITHOUT_DEFLATE();

    // The payload is cut short AND compressedSize is adjusted to match, so the
    // archive is internally consistent and the local-header range check cannot
    // catch it. Only the inflate loop can: it runs out of input without ever
    // reaching Z_STREAM_END.
    ZipEntry entry = deflatedFile("payload.bin", compressibleBlob(4096, 32, 0x9E37u));
    entry.mutatePayload = [](QByteArray &payload) {
        payload.chop(qMax<qsizetype>(1, payload.size() / 4));
    };
    QVERIFY(writeZip(m_zip, {entry}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::Truncated);
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::refusesABadCrcOverInflatedOutput()
{
    SKIP_WITHOUT_DEFLATE();

    // The stream is valid and inflates to exactly the declared length; only
    // the checksum disagrees. This is the one case that proves the CRC is
    // computed over the INFLATED bytes rather than over the compressed ones.
    ZipEntry entry = deflatedFile("payload.bin", compressibleBlob(2048, 40, 0x77u));
    entry.corruptCrc = true;
    QVERIFY(writeZip(m_zip, {entry}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::ChecksumMismatch);
    QVERIFY2(filesUnderStaging().isEmpty(),
             qPrintable(filesUnderStaging().join(QLatin1Char(' '))));
}

void UpdaterZipSafetyTest::refusesADeflateBombAtTheDefaultRatioCap()
{
    SKIP_WITHOUT_DEFLATE();

    // 2 MiB of nothing: the classic highly-compressible member. The refusal
    // must come from the SHIPPED limit, so the test asserts the fixture really
    // does exceed ArchiveLimits::maxCompressionRatio before extracting with
    // the defaults — otherwise a future zlib that compresses worse would let
    // this pass for the wrong reason.
    const ArchiveLimits defaults;
    const QByteArray bomb(2 * 1024 * 1024, '\0');
    const QByteArray compressed = deflateRaw(bomb);
    QVERIFY(!compressed.isEmpty());
    QVERIFY2(qint64(bomb.size())
                 > qint64(compressed.size()) * defaults.maxCompressionRatio,
             qPrintable(QStringLiteral("fixture ratio %1:1 does not exceed the "
                                       "%2:1 cap")
                            .arg(bomb.size() / qMax<qsizetype>(1, compressed.size()))
                            .arg(defaults.maxCompressionRatio)));

    QVERIFY(writeZip(m_zip, {deflatedFile("bomb.bin", bomb)}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::CompressionRatioExceeded);
    // Refused on the declared metadata alone: not one byte was inflated.
    QCOMPARE(result.bytesWritten, qint64(0));
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::acceptsAnEntryAtTheCompressionRatioCap()
{
    SKIP_WITHOUT_DEFLATE();

    // The guard refuses when uncompressed > compressed * maxCompressionRatio,
    // so the smallest cap that still accepts this entry is ceil(u / c). Both
    // sides of that boundary are derived from the payload the fixture really
    // produces rather than from a number that would drift.
    const QByteArray plain = compressibleBlob(1024, 300, 0xC0FFEEu); // 300 KiB
    const QByteArray compressed = deflateRaw(plain);
    QVERIFY(!compressed.isEmpty());
    const int exactRatio = static_cast<int>(
        (qint64(plain.size()) + compressed.size() - 1) / compressed.size());
    QVERIFY(exactRatio > 1);
    // Realistic content: the shipped cap would accept it too.
    QVERIFY(exactRatio <= ArchiveLimits().maxCompressionRatio);

    QVERIFY(writeZip(m_zip, {deflatedFile("data.bin", plain)}));

    ArchiveLimits limits;
    limits.maxCompressionRatio = exactRatio;
    ArchiveResult result = extractZipSafely(m_zip, m_staging, limits);
    QVERIFY2(result.ok(), archiveErrorName(result.error));
    QFile out(QDir(m_staging).absoluteFilePath(QStringLiteral("data.bin")));
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), plain);
    out.close();

    // One step tighter and the very same archive is refused.
    resetStaging();
    limits.maxCompressionRatio = exactRatio - 1;
    result = extractZipSafely(m_zip, m_staging, limits);
    QCOMPARE(result.error, ArchiveError::CompressionRatioExceeded);
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::refusesADeflatedTraversalEntry()
{
    SKIP_WITHOUT_DEFLATE();

    // The path verdict must be reached before the method is ever consulted:
    // compression is not a way around the traversal check.
    QVERIFY(writeZip(m_zip, {
        deflatedFile("../escape.txt", compressibleBlob(1024, 16, 0x2Bu)),
    }));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::UnsafeEntryPath);
    QVERIFY(filesUnderStaging().isEmpty());
    QVERIFY(!QFileInfo::exists(
        QDir(m_dir.path()).absoluteFilePath(QStringLiteral("escape.txt"))));

    // Listing refuses it on the same verdict, and still writes nothing.
    QStringList names;
    const ArchiveResult listed = listZipEntries(m_zip, m_staging, &names);
    QCOMPARE(listed.error, ArchiveError::UnsafeEntryPath);
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::refusesADeflatedSymlinkEntry()
{
    SKIP_WITHOUT_DEFLATE();

    // The link target is the member's content, so a symlink entry can be
    // deflated like any other. The unix mode still decides.
    ZipEntry entry = symlinkEntry("config", QByteArray("/etc/shadow"));
    entry.method = 8;
    QVERIFY(writeZip(m_zip, {entry}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::SymlinkEntryRejected);
    QVERIFY(filesUnderStaging().isEmpty());
    QVERIFY(!QFileInfo(QDir(m_staging).absoluteFilePath(QStringLiteral("config")))
                 .isSymLink());
}

QTEST_GUILESS_MAIN(UpdaterZipSafetyTest)
#include "UpdaterZipSafetyTest.moc"
