
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
    quint16 versionMadeBy = 0x0300;
    quint32 externalAttributes = 0;
    bool corruptCrc = false;
    quint16 method = 0;
    qint64 declaredUncompressedSize = -1;
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
                return false;
        } else {
            return false;
        }
        if (entry.mutatePayload)
            entry.mutatePayload(payload);

        const quint32 compressed = static_cast<quint32>(payload.size());
        const quint32 size = static_cast<quint32>(
            entry.declaredUncompressedSize >= 0 ? entry.declaredUncompressedSize
                                                : entry.contents.size());

        constexpr quint16 kUtf8NameFlag = 0x0800;

        body.append("PK\x03\x04", 4);
        appendU16(body, 20);
        appendU16(body, kUtf8NameFlag);
        appendU16(body, entry.method);
        appendU16(body, 0);
        appendU16(body, 0);
        appendU32(body, crc);
        appendU32(body, compressed);
        appendU32(body, size);
        appendU16(body, static_cast<quint16>(entry.name.size()));
        appendU16(body, 0);
        body.append(entry.name);
        body.append(payload);

        central.append("PK\x01\x02", 4);
        appendU16(central, entry.versionMadeBy);
        appendU16(central, 20);
        appendU16(central, kUtf8NameFlag);
        appendU16(central, entry.method);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU32(central, crc);
        appendU32(central, compressed);
        appendU32(central, size);
        appendU16(central, static_cast<quint16>(entry.name.size()));
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0);
        appendU16(central, 0);
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

}

class UpdaterZipSafetyTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void hostileEntryNamesAreRejected_data();
    void hostileEntryNamesAreRejected();
    void benignEntryNamesAreAccepted_data();
    void benignEntryNamesAreAccepted();
    void rootMustBeAbsolute();
    void symlinkedStagingRootStillContains();
    void resolvedDestinationIsEmptyForUnsafeNames();
    void pathTraversingASymlinkIsDetected();

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
    QVERIFY(resolvedArchiveDestination(name, m_staging).isEmpty());
}

void UpdaterZipSafetyTest::benignEntryNamesAreAccepted_data()
{
    QTest::addColumn<QString>("name");
    QTest::newRow("plain") << QStringLiteral("Lightning.exe");
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
    QVERIFY(pathTraversesSymlink(m_staging, QStringLiteral("/etc/passwd")));
#endif
}

void UpdaterZipSafetyTest::extractsAStoredArchive()
{
    QVERIFY(writeZip(m_zip, {
        regularFile("Lightning.exe", QByteArray("MZ binary payload")),
        regularFile("plugins/platforms/qwindows.dll", QByteArray("dll bytes")),
        regularFile("ištekliai/ąčę (1).txt", QString::fromUtf8("labas").toUtf8()),
    }));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY2(result.ok(), archiveErrorName(result.error));
    QCOMPARE(result.entriesWritten, 3);

    const QDir root(m_staging);
    QFile exe(root.absoluteFilePath(QStringLiteral("Lightning.exe")));
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
        regularFile("Lightning.exe", QByteArray("ok")),
        regularFile("../pwned.txt", QByteArray("owned")),
    }));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QVERIFY(!result.ok());
    QCOMPARE(result.error, ArchiveError::UnsafeEntryPath);
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
    QVERIFY(writeZip(m_zip, {
        regularFile("Lightning.exe", QByteArray("ok")),
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
    ZipEntry entry = regularFile("Lightning.exe", QByteArray("payload"));
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
    QVERIFY(file.resize(file.size() - 8));
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
        regularFile("Lightning.exe", QByteArray("ok")),
        regularFile("data/x.txt", QByteArray("ok")),
    }));
    QStringList names;
    ArchiveResult listed = listZipEntries(m_zip, m_staging, &names);
    QVERIFY2(listed.ok(), archiveErrorName(listed.error));
    QCOMPARE(names, QStringList({QStringLiteral("Lightning.exe"),
                                 QStringLiteral("data/x.txt")}));
    QVERIFY(QDir(m_staging).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                .isEmpty());

    QFile::remove(m_zip);
    QVERIFY(writeZip(m_zip, {regularFile("../escape.txt", QByteArray("x"))}));
    names.clear();
    listed = listZipEntries(m_zip, m_staging, &names);
    QVERIFY(!listed.ok());
    QCOMPARE(listed.error, ArchiveError::UnsafeEntryPath);
}

#define SKIP_WITHOUT_DEFLATE()                                                 \
    do {                                                                       \
        if (!kTestBuildHasZlib || !deflateSupportAvailable())                  \
            QSKIP("built without zlib: deflated entries cannot be exercised"); \
    } while (false)

void UpdaterZipSafetyTest::extractsADeflatedArchive()
{
    SKIP_WITHOUT_DEFLATE();

    const QByteArray exe = compressibleBlob(4096, 32, 0x5EEDu);
    const QByteArray dll = compressibleBlob(1024, 200, 0xBEEFu);
    QByteArray text;
    for (int line = 0; line < 2000; ++line) {
        text += QStringLiteral("ąčę %1 Lightning portable build line\n")
                    .arg(line, 8, 10, QLatin1Char('0'))
                    .toUtf8();
    }

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
        deflatedFile("Lightning.exe", exe),
        deflatedFile("plugins/platforms/qwindows.dll", dll),
        deflatedFile("share/ištekliai/readme.txt", text),
    }));

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
    QCOMPARE(readBack(QStringLiteral("Lightning.exe")), exe);
    QCOMPARE(readBack(QStringLiteral("plugins/platforms/qwindows.dll")), dll);
    QCOMPARE(readBack(QStringLiteral("share/ištekliai/readme.txt")), text);

    QVERIFY(QFileInfo(root.absoluteFilePath(QStringLiteral("plugins/platforms")))
                .isDir());
    QCOMPARE(filesUnderStaging().size(), qsizetype(3));
}

void UpdaterZipSafetyTest::extractsAMixedStoredAndDeflatedArchive()
{
    SKIP_WITHOUT_DEFLATE();

    const QByteArray stored = QByteArray("incompressible-enough\n");
    const QByteArray deflated = compressibleBlob(2048, 64, 0xA11CEu);

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

    const QByteArray real = compressibleBlob(4096, 64, 0xD0D0u);
    const qint64 lie = 96 * 1024;

    ZipEntry entry = deflatedFile("payload.bin", real);
    entry.declaredUncompressedSize = lie;
    QVERIFY(writeZip(m_zip, {entry}));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::EntrySizeExceeded);
    QVERIFY2(filesUnderStaging().isEmpty(),
             qPrintable(filesUnderStaging().join(QLatin1Char(' '))));
    QCOMPARE(result.entriesWritten, 0);
}

void UpdaterZipSafetyTest::refusesAnInvalidDeflateBlockType()
{
    SKIP_WITHOUT_DEFLATE();

    ZipEntry entry = deflatedFile("payload.bin", compressibleBlob(1024, 32, 1u));
    entry.mutatePayload = [](QByteArray &payload) {
        if (payload.isEmpty())
            return;
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
    QCOMPARE(result.bytesWritten, qint64(0));
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::acceptsAnEntryAtTheCompressionRatioCap()
{
    SKIP_WITHOUT_DEFLATE();

    const QByteArray plain = compressibleBlob(1024, 300, 0xC0FFEEu);
    const QByteArray compressed = deflateRaw(plain);
    QVERIFY(!compressed.isEmpty());
    const int exactRatio = static_cast<int>(
        (qint64(plain.size()) + compressed.size() - 1) / compressed.size());
    QVERIFY(exactRatio > 1);
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

    resetStaging();
    limits.maxCompressionRatio = exactRatio - 1;
    result = extractZipSafely(m_zip, m_staging, limits);
    QCOMPARE(result.error, ArchiveError::CompressionRatioExceeded);
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::refusesADeflatedTraversalEntry()
{
    SKIP_WITHOUT_DEFLATE();

    QVERIFY(writeZip(m_zip, {
        deflatedFile("../escape.txt", compressibleBlob(1024, 16, 0x2Bu)),
    }));

    const ArchiveResult result = extractZipSafely(m_zip, m_staging);
    QCOMPARE(result.error, ArchiveError::UnsafeEntryPath);
    QVERIFY(filesUnderStaging().isEmpty());
    QVERIFY(!QFileInfo::exists(
        QDir(m_dir.path()).absoluteFilePath(QStringLiteral("escape.txt"))));

    QStringList names;
    const ArchiveResult listed = listZipEntries(m_zip, m_staging, &names);
    QCOMPARE(listed.error, ArchiveError::UnsafeEntryPath);
    QVERIFY(filesUnderStaging().isEmpty());
}

void UpdaterZipSafetyTest::refusesADeflatedSymlinkEntry()
{
    SKIP_WITHOUT_DEFLATE();

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
