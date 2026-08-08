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

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

} // namespace

class GifStarredStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void starWritesFileAndModelRow();
    void reStarringSameBytesDedupsByHash();
    void rejectsNonGifBytes();
    void itemCapRefusesRatherThanEvicts();
    void byteCapRefusesRatherThanEvicts();
    void unstarDeletesFileAndRow();
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

void GifStarredStoreTest::rejectsNonGifBytes()
{
    QTemporaryDir dir;
    GifStarredStore store;
    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.openFor(dir.path());

    store.starBytes(QStringLiteral("mk"), QByteArrayLiteral("<html>not a gif</html>"));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("not_a_gif"));
    QCOMPARE(store.count(), 0);
    QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.gif")}).isEmpty());
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

QTEST_MAIN(GifStarredStoreTest)
#include "GifStarredStoreTest.moc"
