// GifStarredStore, the client-local saved-image store: content-hash dedup,
// item/byte caps that refuse rather than silently evict, account-scope
// isolation between openFor() directories, unstar deleting the file, stale or
// tampered index entries never surfacing an unplayable tile, and the
// session-only mediaKey->hash mapping behind the hover star's state. The
// picker's QML wiring is covered by GifPickerRedesignContractTest and
// QmlBindingContractTest.

#include "gif/GifResponseParser.h"
#include "gif/GifStarredStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

// A minimal real GIF header: magic plus logical-screen width/height (10 bytes,
// the smallest gif::validateGifBytes accepts), with an optional tail so equal
// dimensions can still hash differently.
QByteArray makeGif(int w, int h, const QByteArray &tail = {})
{
    QByteArray b = "GIF89a";
    b.append(static_cast<char>(w & 0xFF));
    b.append(static_cast<char>((w >> 8) & 0xFF));
    b.append(static_cast<char>(h & 0xFF));
    b.append(static_cast<char>((h >> 8) & 0xFF));
    b.append(tail);
    return b;
}

// Minimal byte-level fixtures for PNG, JPEG and WebP: only the header fields
// gif::validateRasterBytes reads (no checksums or pixel data). Hand-built
// because this target links only Qt6::Core and Qt6::Test (QImage::save needs
// Qt6::Gui).
QByteArray be32(quint32 v)
{
    QByteArray r(4, '\0');
    r[0] = static_cast<char>((v >> 24) & 0xFF);
    r[1] = static_cast<char>((v >> 16) & 0xFF);
    r[2] = static_cast<char>((v >> 8) & 0xFF);
    r[3] = static_cast<char>(v & 0xFF);
    return r;
}

// PNG: 8-byte signature plus the IHDR chunk (length 13, width/height big-endian,
// 5 bytes of depth/colour/compression/filter/interlace and a CRC, both zeroed
// since pngDims() checks neither).
QByteArray makePng(int w, int h)
{
    QByteArray b;
    b.append(static_cast<char>(0x89));
    b += "PNG\r\n";
    b.append(static_cast<char>(0x1a));
    b += "\n";
    b += be32(13);
    b += "IHDR";
    b += be32(static_cast<quint32>(w));
    b += be32(static_cast<quint32>(h));
    b += QByteArray(5, '\0'); // depth/colortype/compression/filter/interlace
    b += QByteArray(4, '\0'); // CRC (unchecked)
    return b;
}

// JPEG: SOI, then a single SOF0 marker segment (Lf=11: precision + height +
// width + one component descriptor), then EOI.
QByteArray makeJpeg(int w, int h)
{
    QByteArray b;
    b.append(static_cast<char>(0xFF)); b.append(static_cast<char>(0xD8)); // SOI
    b.append(static_cast<char>(0xFF)); b.append(static_cast<char>(0xC0)); // SOF0
    b.append(static_cast<char>(0x00)); b.append(static_cast<char>(0x0B)); // Lf=11
    b.append(static_cast<char>(0x08));                                   // precision
    b.append(static_cast<char>((h >> 8) & 0xFF));
    b.append(static_cast<char>(h & 0xFF));
    b.append(static_cast<char>((w >> 8) & 0xFF));
    b.append(static_cast<char>(w & 0xFF));
    b.append(static_cast<char>(0x01));                                   // Nf=1
    b.append(static_cast<char>(0x01));
    b.append(static_cast<char>(0x11));
    b.append(static_cast<char>(0x00));                                   // component
    b.append(static_cast<char>(0xFF)); b.append(static_cast<char>(0xD9)); // EOI
    return b;
}

// WebP: 12-byte RIFF/WEBP container + a single VP8L chunk (signature 0x2F +
// the packed 14-bit-width-1/14-bit-height-1 little-endian field).
QByteArray makeWebp(int w, int h)
{
    QByteArray b;
    b += "RIFF";
    b += QByteArray(4, '\0'); // file size (unchecked)
    b += "WEBP";
    b += "VP8L";
    b += QByteArray(4, '\0'); // chunk size (unchecked)
    b.append(static_cast<char>(0x2F));
    const quint32 v = (static_cast<quint32>(w - 1) & 0x3FFF)
        | ((static_cast<quint32>(h - 1) & 0x3FFF) << 14);
    b.append(static_cast<char>(v & 0xFF));
    b.append(static_cast<char>((v >> 8) & 0xFF));
    b.append(static_cast<char>((v >> 16) & 0xFF));
    b.append(static_cast<char>((v >> 24) & 0xFF));
    return b;
}

} // namespace

class GifStarredStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void starWritesFileAndModelRow();
    void reStarringSameBytesDedupsByHash();
    void rejectsHtmlBytes();
    void itemCapRefusesRatherThanEvicts();
    void byteCapRefusesRatherThanEvicts();
    void unstarDeletesFileAndRow();
    void reStarringAlreadyIndexedHashReportsAlreadyStarredCategory();
    void unstarUnknownHashIsNoop();
    void accountScopeIsolation();
    void closingClearsRowsWithoutTouchingDisk();
    void staleIndexEntryIsPrunedOnOpen();
    void sessionMediaKeyMappingDrivesUnstarByMediaKey();

    void reStarringAfterFileWasManuallyDeletedRewritesIt();
    void openForSweepsOrphanedFilesNotInIndex();
    void oversizedFileOnDiskRejectedByReadBytes();
    void refusesSymlinkedStoreDirectory();
    void refusesWhenAccountRootParentIsSymlinked();
    void storeDirectoryIsOwnerOnlyAfterFirstWrite();
    void directoryIsNotCreatedUntilFirstWrite();
    void clearAllDeletesEveryFileAndRow();
    void sessionMapIsAlreadyClearedWhenCountChangedFires();
    void countAndTotalBytesPropertiesAreReactive();
    void unstarEmitsFeedbackOnlyWhenSomethingRemoved();
    void categoryMessagesAreTranslatedNeverRawTokens();

    // Raster formats (GIF/PNG/JPEG/WebP) decided from bytes, never from a
    // claim; see gif::validateRasterBytes.
    void legacyIndexEntryWithNoExtFieldLoadsAndSendsAsGif();
    void hostileExtInIndexNeverReachesAPath();
    void savesPngBytesUnderThePngSuffix();
    void savesJpegBytesUnderTheJpgSuffix();
    void savesWebpBytesUnderTheWebpSuffix();
    void neverGuessesAnExtensionFromAnythingButTheBytes();
    void rejectsSvgBytes();
    void orphanSweepRemovesStrayFilesOfEveryFormat();
    void unstarRemovesTheCorrectFormatFile();
    void accountScopeIsolationHoldsAcrossFormats();
    void capsRefuseRegardlessOfFormat();
};

void GifStarredStoreTest::starWritesFileAndModelRow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GifStarredStore store;
    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.openFor(dir.path());
    QVERIFY(store.isOpen());

    const QByteArray gif = makeGif(200, 150);
    store.starBytes(QStringLiteral("mk1"), gif);

    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(1).toBool());
    QCOMPARE(store.count(), 1);
    QCOMPARE(store.model()->rowCount(), 1);
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    QCOMPARE(hash.size(), 64);
    QVERIFY(QFileInfo::exists(dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif")));
    QVERIFY(!store.source(hash).isEmpty());
    QCOMPARE(store.readBytes(hash), gif);
}

void GifStarredStoreTest::reStarringSameBytesDedupsByHash()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    const QByteArray gif = makeGif(64, 64);
    store.starBytes(QStringLiteral("mkA"), gif);
    store.starBytes(QStringLiteral("mkB"), gif); // different message, same content
    QCOMPARE(store.count(), 1); // no duplicate row or file
    QVERIFY(store.isStarredThisSession(QStringLiteral("mkA")));
    QVERIFY(store.isStarredThisSession(QStringLiteral("mkB")));
}

// The first star of a hash reports plain success; re-starring an already
// indexed hash (reachable via AppController::isChatGifStarred's durable
// check) reports category "already_starred" with a translated message, never
// a raw token.
void GifStarredStoreTest::reStarringAlreadyIndexedHashReportsAlreadyStarredCategory()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    const QByteArray gif = makeGif(64, 64);

    QSignalSpy first(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mkA"), gif);
    QCOMPARE(first.count(), 1);
    QVERIFY(first.at(0).at(1).toBool());
    QCOMPARE(first.at(0).at(2).toString(), QString()); // plain success
    QCOMPARE(first.at(0).at(3).toString(), QString());

    QSignalSpy second(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mkB"), gif); // same content, different key
    QCOMPARE(second.count(), 1);
    QVERIFY(second.at(0).at(1).toBool()); // still `ok`
    QCOMPARE(second.at(0).at(2).toString(), QStringLiteral("already_starred"));
    const QString message = second.at(0).at(3).toString();
    QVERIFY(!message.isEmpty());
    QVERIFY(!message.contains(QStringLiteral("already_starred"))); // translated, not raw
    QCOMPARE(store.count(), 1); // still no duplicate row or file
}

// HTML matches none of the four supported magics, so starBytes() (via
// gif::validateRasterBytes) refuses it as "unsupported_format", writes
// nothing and reports a failure. categoryMessagesAreTranslatedNeverRawTokens
// proves the token never reaches the UI.
void GifStarredStoreTest::rejectsHtmlBytes()
{
    QTemporaryDir dir;
    GifStarredStore store;
    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.openFor(dir.path());

    store.starBytes(QStringLiteral("mk"), QByteArrayLiteral("<html>not a gif</html>"));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("unsupported_format"));
    QCOMPARE(store.count(), 0);
    QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.gif")}).isEmpty());
}

void GifStarredStoreTest::rejectsSvgBytes()
{
    // SVG is deliberately unsupported: untrusted SVG must never reach the
    // saved store.
    QTemporaryDir dir;
    GifStarredStore store;
    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.openFor(dir.path());

    store.starBytes(QStringLiteral("mk"),
                    QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\">"
                                      "<script>alert(1)</script></svg>"));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("unsupported_format"));
    QCOMPARE(store.count(), 0);
    QVERIFY(QDir(dir.path()).entryList().size() <= 2); // "." / ".." only — no file written
}

void GifStarredStoreTest::itemCapRefusesRatherThanEvicts()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.setCapsForTest(/*maxItems=*/2, /*maxTotalBytes=*/64LL * 1024 * 1024);

    store.starBytes(QStringLiteral("mk1"), makeGif(10, 10, "a"));
    store.starBytes(QStringLiteral("mk2"), makeGif(10, 10, "b"));
    QCOMPARE(store.count(), 2);
    const QString firstHash =
        store.model()->get(0).value(QStringLiteral("gifId")).toString();

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk3"), makeGif(10, 10, "c"));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cap_items"));
    // Still exactly 2: the first item was not evicted for the third.
    QCOMPARE(store.count(), 2);
    QVERIFY(store.model()->contains(QStringLiteral("local"), firstHash));
}

void GifStarredStoreTest::byteCapRefusesRatherThanEvicts()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    // Small caps so the test allocates only a few bytes.
    store.setCapsForTest(/*maxItems=*/200, /*maxTotalBytes=*/20);

    store.starBytes(QStringLiteral("mk1"), makeGif(10, 10, QByteArray(10, 'x'))); // 20 bytes
    QCOMPARE(store.count(), 1);
    const QString firstHash =
        store.model()->get(0).value(QStringLiteral("gifId")).toString();

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk2"), makeGif(10, 10, QByteArray(10, 'y')));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cap_bytes"));
    QCOMPARE(store.count(), 1);
    QVERIFY(store.model()->contains(QStringLiteral("local"), firstHash));
}

void GifStarredStoreTest::unstarDeletesFileAndRow()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("mk"), makeGif(20, 20));
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    const QString path = dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif");
    QVERIFY(QFileInfo::exists(path));

    store.unstar(hash);
    QCOMPARE(store.count(), 0);
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(store.source(hash).isEmpty());
}

void GifStarredStoreTest::unstarUnknownHashIsNoop()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("mk"), makeGif(20, 20));
    QCOMPARE(store.count(), 1);
    store.unstar(QStringLiteral("not-a-real-hash"));
    QCOMPARE(store.count(), 1); // untouched
}

void GifStarredStoreTest::accountScopeIsolation()
{
    QTemporaryDir dirA;
    QTemporaryDir dirB;
    GifStarredStore store;
    store.openFor(dirA.path());
    store.starBytes(QStringLiteral("mk"), makeGif(30, 30));
    QCOMPARE(store.count(), 1);

    // Another account's directory shows that account's (empty) store, never
    // account A's rows.
    store.openFor(dirB.path());
    QCOMPARE(store.count(), 0);
    QVERIFY(!store.isStarredThisSession(QStringLiteral("mk"))); // session map reset too

    // Switching back reloads account A's index from disk.
    store.openFor(dirA.path());
    QCOMPARE(store.count(), 1);
}

void GifStarredStoreTest::closingClearsRowsWithoutTouchingDisk()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("mk"), makeGif(40, 40));
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    const QString path = dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif");

    store.close();
    QVERIFY(!store.isOpen());
    QCOMPARE(store.count(), 0);          // no stale rows visible while closed
    QVERIFY(QFileInfo::exists(path));    // logout/close never deletes the file

    store.openFor(dir.path());
    QCOMPARE(store.count(), 1);          // reappears once reopened
}

void GifStarredStoreTest::staleIndexEntryIsPrunedOnOpen()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path()));
    QSettings settings(dir.path() + QStringLiteral("/index.ini"),
                       QSettings::IniFormat);
    // A tampered or stale index entry: a well-formed hash whose file does not
    // exist.
    const QString fakeHash = QString(64, QLatin1Char('a'));
    settings.setValue(QStringLiteral("gif/starred"),
                      QStringLiteral("[{\"provider\":\"local\",\"id\":\"%1\","
                                     "\"w\":10,\"h\":10,\"bytes\":10}]")
                          .arg(fakeHash));
    settings.sync();

    GifStarredStore store;
    store.openFor(dir.path());
    QCOMPARE(store.count(), 0); // dropped — the file does not exist
}

void GifStarredStoreTest::sessionMediaKeyMappingDrivesUnstarByMediaKey()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("m1"), makeGif(50, 50));
    QVERIFY(store.isStarredThisSession(QStringLiteral("m1")));
    QVERIFY(!store.isStarredThisSession(QStringLiteral("unrelated")));

    store.unstarByMediaKey(QStringLiteral("unrelated")); // no mapping — no-op
    QCOMPARE(store.count(), 1);

    store.unstarByMediaKey(QStringLiteral("m1"));
    QCOMPARE(store.count(), 0);
    QVERIFY(!store.isStarredThisSession(QStringLiteral("m1")));
}

void GifStarredStoreTest::reStarringAfterFileWasManuallyDeletedRewritesIt()
{
    // A re-star must not skip the write because the index still has the hash:
    // the file may have been removed by something other than unstar().
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    const QByteArray gif = makeGif(20, 20);
    store.starBytes(QStringLiteral("mk1"), gif);
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    const QString path = dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif");
    QVERIFY(QFile::remove(path));
    QVERIFY(store.model()->hasHash(hash)); // index still has the row

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk2"), gif); // identical bytes, same hash
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(1).toBool());
    QVERIFY(QFileInfo::exists(path)); // rewritten, not silently skipped
    QCOMPARE(store.readBytes(hash), gif);
}

void GifStarredStoreTest::openForSweepsOrphanedFilesNotInIndex()
{
    // An on-disk file no index row references (crash between write and
    // registration, hand-edited index) is swept by openFor() rather than
    // escaping the cap.
    QTemporaryDir dir;
    QString keptHash;
    {
        GifStarredStore store;
        store.openFor(dir.path());
        store.starBytes(QStringLiteral("mk"), makeGif(10, 10, "a"));
        keptHash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    }
    const QString orphanHash = QString(64, QLatin1Char('f'));
    const QString orphanPath =
        dir.path() + QLatin1Char('/') + orphanHash + QStringLiteral(".gif");
    QFile orphan(orphanPath);
    QVERIFY(orphan.open(QIODevice::WriteOnly));
    orphan.write(makeGif(5, 5));
    orphan.close();
    QVERIFY(QFileInfo::exists(orphanPath));

    GifStarredStore reopened;
    reopened.openFor(dir.path());
    QCOMPARE(reopened.count(), 1);
    QVERIFY(reopened.model()->hasHash(keptHash));
    QVERIFY(!QFileInfo::exists(orphanPath)); // swept
    QVERIFY(QFileInfo::exists(
        dir.path() + QLatin1Char('/') + keptHash + QStringLiteral(".gif")));
}

void GifStarredStoreTest::oversizedFileOnDiskRejectedByReadBytes()
{
    // readBytes() bounds the read even though the file was validated at write
    // time; it may have been replaced since.
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("mk"), makeGif(10, 10));
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    const QString path = dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif");

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray huge(static_cast<int>(gif::kMaxGifBytes) + 1024, 'x');
    QCOMPARE(f.write(huge), qint64(huge.size()));
    f.close();

    QVERIFY(store.readBytes(hash).isEmpty());
}

void GifStarredStoreTest::refusesSymlinkedStoreDirectory()
{
#ifdef Q_OS_WIN
    QSKIP("symlink creation needs elevated privileges on Windows");
#endif
    QTemporaryDir realDir;
    QTemporaryDir linkParent;
    const QString linkPath = linkParent.path() + QStringLiteral("/starred-gifs");
    QVERIFY(QFile::link(realDir.path(), linkPath));

    GifStarredStore store;
    store.openFor(linkPath);
    QVERIFY(!store.isOpen());
    store.starBytes(QStringLiteral("mk"), makeGif(10, 10));
    QCOMPARE(store.count(), 0); // never wrote through the link
}

void GifStarredStoreTest::refusesWhenAccountRootParentIsSymlinked()
{
#ifdef Q_OS_WIN
    QSKIP("symlink creation needs elevated privileges on Windows");
#endif
    QTemporaryDir realAccountRoot;
    QTemporaryDir linkParent;
    const QString linkedAccountRoot = linkParent.path() + QStringLiteral("/account");
    QVERIFY(QFile::link(realAccountRoot.path(), linkedAccountRoot));
    const QString starredDir = linkedAccountRoot + QStringLiteral("/starred-gifs");

    GifStarredStore store;
    store.openFor(starredDir);
    QVERIFY(!store.isOpen());
}

void GifStarredStoreTest::storeDirectoryIsOwnerOnlyAfterFirstWrite()
{
#ifdef Q_OS_WIN
    QSKIP("POSIX permission bits are not meaningful on Windows");
#endif
    QTemporaryDir dir;
    const QString starredDir = dir.path() + QStringLiteral("/starred-gifs");
    GifStarredStore store;
    store.openFor(starredDir);
    store.starBytes(QStringLiteral("mk"), makeGif(10, 10));

    const QFileDevice::Permissions perms = QFileInfo(starredDir).permissions();
    QVERIFY(!(perms & QFileDevice::ReadGroup));
    QVERIFY(!(perms & QFileDevice::WriteGroup));
    QVERIFY(!(perms & QFileDevice::ReadOther));
    QVERIFY(!(perms & QFileDevice::WriteOther));
}

void GifStarredStoreTest::directoryIsNotCreatedUntilFirstWrite()
{
    // An account that never stars anything gets no empty directory on login.
    QTemporaryDir dir;
    const QString starredDir = dir.path() + QStringLiteral("/starred-gifs");
    GifStarredStore store;
    store.openFor(starredDir);
    QVERIFY(store.isOpen());
    QVERIFY(!QDir(starredDir).exists());

    store.starBytes(QStringLiteral("mk"), makeGif(10, 10));
    QVERIFY(QDir(starredDir).exists()); // created on the actual write
}

void GifStarredStoreTest::clearAllDeletesEveryFileAndRow()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("mk1"), makeGif(10, 10, "a"));
    store.starBytes(QStringLiteral("mk2"), makeGif(10, 10, "b"));
    QCOMPARE(store.count(), 2);
    const QString h1 = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    const QString h2 = store.model()->get(1).value(QStringLiteral("gifId")).toString();

    store.clearAll();

    QCOMPARE(store.count(), 0);
    QVERIFY(!QFileInfo::exists(dir.path() + QLatin1Char('/') + h1 + QStringLiteral(".gif")));
    QVERIFY(!QFileInfo::exists(dir.path() + QLatin1Char('/') + h2 + QStringLiteral(".gif")));
    QVERIFY(!store.isStarredThisSession(QStringLiteral("mk1")));
    QVERIFY(!store.isStarredThisSession(QStringLiteral("mk2")));
}

// Ordering contract: consumers such as the hover star refresh from
// countChanged, which is the only signal clearAll emits. If the session map
// were still populated inside that handler, the star would stay filled and
// its next activation would re-persist bytes the user just deleted. Asserted
// for both paths that clear mappings.
void GifStarredStoreTest::sessionMapIsAlreadyClearedWhenCountChangedFires()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());

    // --- clearAll(): Settings > "Clear All" ---
    store.starBytes(QStringLiteral("mk1"), makeGif(10, 10, "a"));
    store.starBytes(QStringLiteral("mk2"), makeGif(10, 10, "b"));
    QVERIFY(store.isStarredThisSession(QStringLiteral("mk1")));

    int observations = 0;
    bool staleInsideHandler = false;
    auto conn = QObject::connect(
        &store, &GifStarredStore::countChanged, &store, [&] {
            ++observations;
            if (store.isStarredThisSession(QStringLiteral("mk1"))
                || store.isStarredThisSession(QStringLiteral("mk2")))
                staleInsideHandler = true;
        });

    store.clearAll();
    QObject::disconnect(conn);

    QVERIFY2(observations > 0, "clearAll must emit countChanged at all");
    QVERIFY2(!staleInsideHandler,
             "countChanged fired while the session map still reported the "
             "cleared GIFs as starred — a consumer refreshing from that "
             "signal keeps a filled star that re-persists deleted bytes");

    // --- unstar(): the per-hash path, same ordering rule ---
    store.starBytes(QStringLiteral("mk3"), makeGif(12, 12, "c"));
    const QString h3 =
        store.model()->get(0).value(QStringLiteral("gifId")).toString();
    QVERIFY(store.isStarredThisSession(QStringLiteral("mk3")));

    observations = 0;
    staleInsideHandler = false;
    conn = QObject::connect(
        &store, &GifStarredStore::countChanged, &store, [&] {
            ++observations;
            if (store.isStarredThisSession(QStringLiteral("mk3")))
                staleInsideHandler = true;
        });

    store.unstar(h3);
    QObject::disconnect(conn);

    QVERIFY2(observations > 0, "unstar must emit countChanged at all");
    QVERIFY2(!staleInsideHandler,
             "countChanged fired while the session map still reported the "
             "unstarred GIF as starred");
}

void GifStarredStoreTest::countAndTotalBytesPropertiesAreReactive()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    QSignalSpy countSpy(&store, &GifStarredStore::countChanged);
    QCOMPARE(store.count(), 0);
    QCOMPARE(store.totalBytes(), qint64(0));

    store.starBytes(QStringLiteral("mk"), makeGif(10, 10));

    QVERIFY(countSpy.count() > 0);
    QCOMPARE(store.count(), 1);
    QVERIFY(store.totalBytes() > 0);
}

void GifStarredStoreTest::unstarEmitsFeedbackOnlyWhenSomethingRemoved()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("mk"), makeGif(10, 10));
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();

    QSignalSpy unstarSpy(&store, &GifStarredStore::unstarFinished);
    store.unstar(QStringLiteral("not-a-real-hash-and-wrong-length"));
    QCOMPARE(unstarSpy.count(), 0); // no-op — no feedback for nothing removed

    store.unstar(hash);
    QCOMPARE(unstarSpy.count(), 1);
    QCOMPARE(unstarSpy.at(0).at(0).toString(), hash);
}

void GifStarredStoreTest::categoryMessagesAreTranslatedNeverRawTokens()
{
    // The UI message is never a raw category token, and a cap refusal states
    // the configured limit.
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.setCapsForTest(/*maxItems=*/1, /*maxTotalBytes=*/64LL * 1024 * 1024);
    store.starBytes(QStringLiteral("mk1"), makeGif(10, 10, "a"));

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk2"), makeGif(10, 10, "b")); // cap_items
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cap_items"));
    const QString message = finished.at(0).at(3).toString();
    QVERIFY(!message.isEmpty());
    QVERIFY(!message.contains(QStringLiteral("cap_items")));
    QVERIFY(message.contains(QStringLiteral("1"))); // states the real limit
}

// ---- raster formats ----

// A legacy index row with no "ext" key loads and sends as a GIF with no
// migration pass (see GifStarredStore's class comment and
// GifStoredModel::fromJson).
void GifStarredStoreTest::legacyIndexEntryWithNoExtFieldLoadsAndSendsAsGif()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path()));
    const QByteArray gif = makeGif(12, 12);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(gif, QCryptographicHash::Sha256).toHex());

    // Hand-write what an older build persisted: "<hash>.gif" and an index row
    // with no "ext" key.
    QFile file(dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(gif), qint64(gif.size()));
    file.close();

    QSettings settings(dir.path() + QStringLiteral("/index.ini"),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("gif/starred"),
                      QStringLiteral("[{\"provider\":\"local\",\"id\":\"%1\","
                                     "\"w\":12,\"h\":12,\"bytes\":%2}]")
                          .arg(hash).arg(gif.size()));
    settings.sync();

    GifStarredStore store;
    store.openFor(dir.path());
    // Visible, not pruned as stale: "ext"-less reads as "gif".
    QCOMPARE(store.count(), 1);
    QVERIFY(store.model()->hasHash(hash));
    // source(), readBytes() and sourceExt() follow the same convention.
    QVERIFY(!store.source(hash).isEmpty());
    QCOMPARE(store.readBytes(hash), gif);
    QCOMPARE(store.sourceExt(hash), QStringLiteral("gif"));
}

// The persisted "ext" takes part in file-path construction (unstar's remove,
// readBytes, source's file:// URL), so a hostile or corrupted value,
// including traversal, must collapse to legacy-GIF semantics at read time.
void GifStarredStoreTest::hostileExtInIndexNeverReachesAPath()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path()));
    const QByteArray gif = makeGif(12, 12);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(gif, QCryptographicHash::Sha256).toHex());

    // A decoy OUTSIDE the store directory that a traversal ext would reach.
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString decoy = outside.path() + QStringLiteral("/decoy.txt");
    {
        QFile f(decoy);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("do not delete");
    }
    // Relative traversal from the store dir to the decoy, smuggled as "ext".
    const QString traversal = QStringLiteral("gif/..") +
        QString(QStringLiteral("/..")).repeated(8)
        + outside.path() + QStringLiteral("/decoy.txt");

    QFile file(dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(gif), qint64(gif.size()));
    file.close();

    QSettings settings(dir.path() + QStringLiteral("/index.ini"),
                       QSettings::IniFormat);
    settings.setValue(QStringLiteral("gif/starred"),
                      QStringLiteral("[{\"provider\":\"local\",\"id\":\"%1\","
                                     "\"w\":12,\"h\":12,\"bytes\":%2,"
                                     "\"ext\":\"%3\"}]")
                          .arg(hash).arg(gif.size()).arg(traversal));
    settings.sync();

    GifStarredStore store;
    store.openFor(dir.path());
    // The hostile ext collapsed to legacy GIF: the row survives, resolves as
    // gif, and every path stays inside the store.
    QCOMPARE(store.count(), 1);
    QCOMPARE(store.sourceExt(hash), QStringLiteral("gif"));
    QVERIFY(store.source(hash).contains(hash + QStringLiteral(".gif")));
    QCOMPARE(store.readBytes(hash), gif);
    // unstar removes only the in-store file; the decoy is untouched.
    store.unstar(hash);
    QVERIFY(!QFileInfo::exists(dir.path() + QLatin1Char('/') + hash
                               + QStringLiteral(".gif")));
    QVERIFY(QFileInfo::exists(decoy));
}

void GifStarredStoreTest::savesPngBytesUnderThePngSuffix()
{
    QTemporaryDir dir;
    GifStarredStore store;
    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.openFor(dir.path());

    const QByteArray png = makePng(64, 48);
    store.starBytes(QStringLiteral("mk"), png);

    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(1).toBool());
    QCOMPARE(store.count(), 1);
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    QVERIFY(QFileInfo::exists(dir.path() + QLatin1Char('/') + hash + QStringLiteral(".png")));
    QVERIFY(!QFileInfo::exists(dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif")));
    QCOMPARE(store.sourceExt(hash), QStringLiteral("png"));
    QVERIFY(store.source(hash).endsWith(QStringLiteral(".png")));
    QCOMPARE(store.readBytes(hash), png);
}

void GifStarredStoreTest::savesJpegBytesUnderTheJpgSuffix()
{
    QTemporaryDir dir;
    GifStarredStore store;
    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.openFor(dir.path());

    const QByteArray jpeg = makeJpeg(32, 16);
    store.starBytes(QStringLiteral("mk"), jpeg);

    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(1).toBool());
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    QVERIFY(QFileInfo::exists(dir.path() + QLatin1Char('/') + hash + QStringLiteral(".jpg")));
    QCOMPARE(store.sourceExt(hash), QStringLiteral("jpg"));
    QCOMPARE(store.readBytes(hash), jpeg);
}

void GifStarredStoreTest::savesWebpBytesUnderTheWebpSuffix()
{
    QTemporaryDir dir;
    GifStarredStore store;
    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.openFor(dir.path());

    const QByteArray webp = makeWebp(40, 30);
    store.starBytes(QStringLiteral("mk"), webp);

    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(1).toBool());
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    QVERIFY(QFileInfo::exists(dir.path() + QLatin1Char('/') + hash + QStringLiteral(".webp")));
    QCOMPARE(store.sourceExt(hash), QStringLiteral("webp"));
    QCOMPARE(store.readBytes(hash), webp);
}

// starBytes() takes no filename or MIME; the stored suffix is decided only by
// what gif::validateRasterBytes reads from the bytes, so PNG bytes are never
// written as "<hash>.gif".
void GifStarredStoreTest::neverGuessesAnExtensionFromAnythingButTheBytes()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    const QByteArray png = makePng(8, 8);
    store.starBytes(QStringLiteral("mk-claims-to-be-a-gif"), png);
    QCOMPARE(store.count(), 1);
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    QVERIFY(QFileInfo::exists(
        dir.path() + QLatin1Char('/') + hash + QStringLiteral(".png")));
    QVERIFY(!QFileInfo::exists(
        dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif")));
        // Nor under another supported suffix.
    for (const QString &wrongExt : { QStringLiteral("jpg"), QStringLiteral("webp") }) {
        QVERIFY(!QFileInfo::exists(
            dir.path() + QLatin1Char('/') + hash + QLatin1Char('.') + wrongExt));
    }
}

// Unreferenced *.png/*.jpg/*.webp files are swept like *.gif orphans
// (openForSweepsOrphanedFilesNotInIndex).
void GifStarredStoreTest::orphanSweepRemovesStrayFilesOfEveryFormat()
{
    QTemporaryDir dir;
    QString keptHash;
    {
        GifStarredStore store;
        store.openFor(dir.path());
        store.starBytes(QStringLiteral("mk"), makeGif(10, 10, "a"));
        keptHash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    }
    QStringList orphanPaths;
    const QHash<QString, QChar> hexDigitForExt = {
        { QStringLiteral("png"), QLatin1Char('a') },
        { QStringLiteral("jpg"), QLatin1Char('b') },
        { QStringLiteral("webp"), QLatin1Char('c') },
    };
    for (const QString &ext : { QStringLiteral("png"), QStringLiteral("jpg"),
                                QStringLiteral("webp") }) {
        // A well-formed but unreferenced hash, so the sweep drops it because
        // the index does not know it, not because the name is malformed.
        const QString orphanHash = QString(64, hexDigitForExt.value(ext));
        const QString path =
            dir.path() + QLatin1Char('/') + orphanHash + QLatin1Char('.') + ext;
        QFile orphan(path);
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        orphan.write("x");
        orphan.close();
        orphanPaths << path;
    }

    GifStarredStore reopened;
    reopened.openFor(dir.path());
    QCOMPARE(reopened.count(), 1);
    QVERIFY(reopened.model()->hasHash(keptHash));
    for (const QString &path : std::as_const(orphanPaths))
        QVERIFY(!QFileInfo::exists(path));
    QVERIFY(QFileInfo::exists(
        dir.path() + QLatin1Char('/') + keptHash + QStringLiteral(".gif")));
}

void GifStarredStoreTest::unstarRemovesTheCorrectFormatFile()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("mk"), makePng(20, 20));
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    const QString path = dir.path() + QLatin1Char('/') + hash + QStringLiteral(".png");
    QVERIFY(QFileInfo::exists(path));

    store.unstar(hash);
    QCOMPARE(store.count(), 0);
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(store.source(hash).isEmpty());
    QVERIFY(store.sourceExt(hash).isEmpty());
}

void GifStarredStoreTest::accountScopeIsolationHoldsAcrossFormats()
{
    QTemporaryDir dirA;
    QTemporaryDir dirB;
    GifStarredStore store;
    store.openFor(dirA.path());
    store.starBytes(QStringLiteral("mk"), makeWebp(16, 16));
    QCOMPARE(store.count(), 1);

    store.openFor(dirB.path());
    QCOMPARE(store.count(), 0);

    store.openFor(dirA.path());
    QCOMPARE(store.count(), 1);
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    QCOMPARE(store.sourceExt(hash), QStringLiteral("webp"));
}

void GifStarredStoreTest::capsRefuseRegardlessOfFormat()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.setCapsForTest(/*maxItems=*/1, /*maxTotalBytes=*/64LL * 1024 * 1024);
    store.starBytes(QStringLiteral("mk1"), makePng(10, 10));

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk2"), makeJpeg(10, 10));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cap_items"));
    QCOMPARE(store.count(), 1); // the PNG stays; the JPEG was refused
}

QTEST_MAIN(GifStarredStoreTest)
#include "GifStarredStoreTest.moc"
