// v0.6.6: "star a chat GIF" — the client-local save store. Exercises
// content-hash dedup, the item/byte-size caps that REFUSE rather than
// silently evict a GIF the user chose to keep, account-scope isolation
// (two openFor() directories never see each other's rows), unstar deleting
// the on-disk file, a stale/tampered index entry never surfacing a tile
// that cannot actually be played, and the session-only mediaKey->hash
// mapping the hover-star's filled/outline state depends on. The picker's
// Starred tab binds GifStarredStore::model() (GifStarredModel) directly —
// see GifPickerRedesignContractTest/QmlBindingContractTest for that
// source-level wiring; there is no merged-model class to unit test here
// any more (GifFavoritesMergedModel was removed with the UX rework that
// gave Starred its own picker tab instead of folding it into Favorites).

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

// A minimal, real GIF header: magic + logical-screen-descriptor width/height
// (exactly 10 bytes, the smallest gif::validateGifBytes accepts), plus an
// optional tail so otherwise-identical dimensions still hash differently
// (used to create distinct "GIFs" for cap tests).
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

// 2026-08 media round: minimal, real byte-level fixtures for the three new raster
// formats — deliberately as small as gif::validateRasterBytes' own parsers
// actually require (they read the format's HEADER fields only; none of
// them verify a checksum or decode pixel data), exactly like makeGif()
// above is a minimal GIF, not a fully rendered one. This target links only
// Qt6::Core + Qt6::Test (see CMakeLists.txt), so these are hand-built
// rather than produced via QImage::save (which needs Qt6::Gui — the
// gif-send-controller-test target does link Gui and uses that approach for
// its own PNG fixture instead).
QByteArray be32(quint32 v)
{
    QByteArray r(4, '\0');
    r[0] = static_cast<char>((v >> 24) & 0xFF);
    r[1] = static_cast<char>((v >> 16) & 0xFF);
    r[2] = static_cast<char>((v >> 8) & 0xFF);
    r[3] = static_cast<char>(v & 0xFF);
    return r;
}

// PNG: 8-byte signature + the mandatory IHDR chunk (length=13, "IHDR",
// width/height big-endian, then 5 bytes of bit-depth/color-type/
// compression/filter/interlace and a 4-byte CRC — both left zeroed since
// pngDims() never checks either).
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

    // Review round (CRITICAL-1/HIGH-2/M4/L7/L8/L9/L10/L13/M6) additions.
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

    // 2026-08 media round: raster format generalization (GIF/PNG/JPEG/WebP, byte-decided
    // never trusted from a claim) — see gif::validateRasterBytes.
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

// v0.6.6 review (M1, optional/preferred improvement): the FIRST star of a
// hash reports plain success (category/message both empty, so the UI says
// "Starred."); re-starring the SAME already-indexed hash (the ambiguous-
// activation path AppController::isChatGifStarred's durable check can land
// on when it does not yet know an answer) must say so honestly rather than
// implying a brand new star just happened — category "already_starred",
// with a real translated message, never a raw token.
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

// 2026-08 media round: CHANGED PIN — starBytes() now calls gif::validateRasterBytes (the
// general four-format validator), not gif::validateGifBytes directly. HTML
// bytes match none of GIF/PNG/JPEG/WebP's magic, so the category is now
// "unsupported_format" (validateRasterBytes' own token for "not one of the
// four supported magics"), not the GIF-only validator's "not_a_gif". This
// is a legitimate behavior change, not a loosened assertion: the store
// still refuses the bytes, writes nothing, and reports a real failure —
// only the specific machine-readable token differs, and
// categoryMessagesAreTranslatedNeverRawTokens below still proves it never
// reaches the UI as a raw token either way.
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
    // SVG is deliberately unsupported (CLAUDE.md §6: never render untrusted
    // SVG as active content — it must never even reach the "saved" store).
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
    // Still exactly 2 — the first item was never silently evicted to make
    // room for the third.
    QCOMPARE(store.count(), 2);
    QVERIFY(store.model()->contains(QStringLiteral("local"), firstHash));
}

void GifStarredStoreTest::byteCapRefusesRatherThanEvicts()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    // Small caps so the test allocates only a few KB, not real GiB content.
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

    // Switching to a different account's directory shows THAT account's
    // (empty) store — never account A's rows.
    store.openFor(dirB.path());
    QCOMPARE(store.count(), 0);
    QVERIFY(!store.isStarredThisSession(QStringLiteral("mk"))); // session map reset too

    // Switching back reloads account A's persisted index from disk.
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
    // A tampered/stale index entry: a well-formed hash whose file was never
    // written (or was deleted out from under the index).
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
    // L7: a re-star must not skip the write just because the INDEX still
    // thinks the hash exists — the backing file may have been removed by
    // something other than unstar() (disk cleanup, corruption, etc.).
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
    // M4: a *.gif file on disk that no index row references (crash between
    // write and registration, hand-edited index, ...) must not silently
    // escape the cap or linger forever — openFor() sweeps it.
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
    // L8: readBytes() must bound the read even though the file was
    // validated at write time — it may have been replaced since.
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
    // L21: an account that never stars anything must not get an empty
    // starred-gifs directory materialized on every login.
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

// Ordering contract, not just end state: consumers refresh their own
// "is this starred?" view FROM countChanged (the hover star on a GIF row
// does exactly that, and for clearAll it is the ONLY signal emitted — no
// per-hash unstarFinished). If the session map were still populated while
// that signal ran, the star would stay filled and its next activation
// would RE-STAR the file — re-persisting decrypted bytes the user just
// explicitly deleted. So the map must already read false INSIDE the
// handler, not merely after the call returns. Asserted for both mutation
// paths that clear mappings.
void GifStarredStoreTest::sessionMapIsAlreadyClearedWhenCountChangedFires()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());

    // --- clearAll(): the Settings -> "Clear All" path ---
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

    // --- unstar(): the per-hash path (same ordering rule) ---
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
    // M6: the UI-facing message must never be a raw category token, and a
    // cap refusal must state the actual configured limit.
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

// 2026-08 raster format generalization ────────────────────────────────────

// A LEGACY entry — an index row with no "ext" key at all, as every row was
// persisted before this generalization — must keep loading and sending
// exactly as it always did: no migration/rewrite pass runs, and a bare
// "ext"-less row means "gif" (see GifStarredStore's class comment and
// GifStoredModel::fromJson).
void GifStarredStoreTest::legacyIndexEntryWithNoExtFieldLoadsAndSendsAsGif()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path()));
    const QByteArray gif = makeGif(12, 12);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(gif, QCryptographicHash::Sha256).toHex());

    // Hand-write exactly what a pre-generalization build would have persisted: the
    // GIF file at "<hash>.gif" and an index row with no "ext" key.
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
    // Visible: not pruned as stale (the file DOES exist, once "ext"-less is
    // read as "gif").
    QCOMPARE(store.count(), 1);
    QVERIFY(store.model()->hasHash(hash));
    // Sendable fields: source()/readBytes()/sourceExt() all resolve through
    // the same "ext"-less-means-gif convention.
    QVERIFY(!store.source(hash).isEmpty());
    QCOMPARE(store.readBytes(hash), gif);
    QCOMPARE(store.sourceExt(hash), QStringLiteral("gif"));
}

// review M1: the persisted "ext" participates in file-path construction
// (filePath -> unstar's QFile::remove, readBytes, source's file:// URL), so
// a hostile/corrupted index value — including a traversal-shaped one — must
// collapse to legacy-GIF semantics at read time and never reach a path.
// Old behavior: the raw string was used verbatim.
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
    // The hostile ext collapsed to legacy-GIF semantics: the row survives
    // (its <hash>.gif exists), resolves as gif, and every path stays inside
    // the store.
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

// "Spoofed extension" in spirit: nothing about starBytes()'s call site ever
// claims a format (there is no filename/MIME parameter at all) — the ONLY
// input is bytes, and the ONLY thing that ever decides the stored suffix is
// what gif::validateRasterBytes reads from those bytes. This proves PNG
// bytes are never, under any circumstance, written as "<hash>.gif".
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
    // Not even under a DIFFERENT supported suffix — bytes decided "png" and
    // nothing else was ever written.
    for (const QString &wrongExt : { QStringLiteral("jpg"), QStringLiteral("webp") }) {
        QVERIFY(!QFileInfo::exists(
            dir.path() + QLatin1Char('/') + hash + QLatin1Char('.') + wrongExt));
    }
}

// M4 extended to every supported suffix: a *.png/*.jpg/*.webp file the
// index does not reference must be swept exactly like a *.gif orphan
// already was (openForSweepsOrphanedFilesNotInIndex above).
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
        // A well-formed (valid hex) but unreferenced hash — proves the
        // sweep drops it because the INDEX does not know it, not merely
        // because the fake name fails the hash-format check.
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
