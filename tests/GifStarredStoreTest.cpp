
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

QByteArray be32(quint32 v)
{
    QByteArray r(4, '\0');
    r[0] = static_cast<char>((v >> 24) & 0xFF);
    r[1] = static_cast<char>((v >> 16) & 0xFF);
    r[2] = static_cast<char>((v >> 8) & 0xFF);
    r[3] = static_cast<char>(v & 0xFF);
    return r;
}

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
    b += QByteArray(5, '\0');
    b += QByteArray(4, '\0');
    return b;
}

QByteArray makeJpeg(int w, int h)
{
    QByteArray b;
    b.append(static_cast<char>(0xFF)); b.append(static_cast<char>(0xD8));
    b.append(static_cast<char>(0xFF)); b.append(static_cast<char>(0xC0));
    b.append(static_cast<char>(0x00)); b.append(static_cast<char>(0x0B));
    b.append(static_cast<char>(0x08));
    b.append(static_cast<char>((h >> 8) & 0xFF));
    b.append(static_cast<char>(h & 0xFF));
    b.append(static_cast<char>((w >> 8) & 0xFF));
    b.append(static_cast<char>(w & 0xFF));
    b.append(static_cast<char>(0x01));
    b.append(static_cast<char>(0x01));
    b.append(static_cast<char>(0x11));
    b.append(static_cast<char>(0x00));
    b.append(static_cast<char>(0xFF)); b.append(static_cast<char>(0xD9));
    return b;
}

QByteArray makeWebp(int w, int h)
{
    QByteArray b;
    b += "RIFF";
    b += QByteArray(4, '\0');
    b += "WEBP";
    b += "VP8L";
    b += QByteArray(4, '\0');
    b.append(static_cast<char>(0x2F));
    const quint32 v = (static_cast<quint32>(w - 1) & 0x3FFF)
        | ((static_cast<quint32>(h - 1) & 0x3FFF) << 14);
    b.append(static_cast<char>(v & 0xFF));
    b.append(static_cast<char>((v >> 8) & 0xFF));
    b.append(static_cast<char>((v >> 16) & 0xFF));
    b.append(static_cast<char>((v >> 24) & 0xFF));
    return b;
}

}

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
    store.starBytes(QStringLiteral("mkB"), gif);
    QCOMPARE(store.count(), 1);
    QVERIFY(store.isStarredThisSession(QStringLiteral("mkA")));
    QVERIFY(store.isStarredThisSession(QStringLiteral("mkB")));
}

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
    QCOMPARE(first.at(0).at(2).toString(), QString());
    QCOMPARE(first.at(0).at(3).toString(), QString());

    QSignalSpy second(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mkB"), gif);
    QCOMPARE(second.count(), 1);
    QVERIFY(second.at(0).at(1).toBool());
    QCOMPARE(second.at(0).at(2).toString(), QStringLiteral("already_starred"));
    const QString message = second.at(0).at(3).toString();
    QVERIFY(!message.isEmpty());
    QVERIFY(!message.contains(QStringLiteral("already_starred")));
    QCOMPARE(store.count(), 1);
}

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
    QVERIFY(QDir(dir.path()).entryList().size() <= 2);
}

void GifStarredStoreTest::itemCapRefusesRatherThanEvicts()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.setCapsForTest(2,64LL * 1024 * 1024);

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
    QCOMPARE(store.count(), 2);
    QVERIFY(store.model()->contains(QStringLiteral("local"), firstHash));
}

void GifStarredStoreTest::byteCapRefusesRatherThanEvicts()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.setCapsForTest(200,20);

    store.starBytes(QStringLiteral("mk1"), makeGif(10, 10, QByteArray(10, 'x')));
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
    QCOMPARE(store.count(), 1);
}

void GifStarredStoreTest::accountScopeIsolation()
{
    QTemporaryDir dirA;
    QTemporaryDir dirB;
    GifStarredStore store;
    store.openFor(dirA.path());
    store.starBytes(QStringLiteral("mk"), makeGif(30, 30));
    QCOMPARE(store.count(), 1);

    store.openFor(dirB.path());
    QCOMPARE(store.count(), 0);
    QVERIFY(!store.isStarredThisSession(QStringLiteral("mk")));

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
    QCOMPARE(store.count(), 0);
    QVERIFY(QFileInfo::exists(path));

    store.openFor(dir.path());
    QCOMPARE(store.count(), 1);
}

void GifStarredStoreTest::staleIndexEntryIsPrunedOnOpen()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path()));
    QSettings settings(dir.path() + QStringLiteral("/index.ini"),
                       QSettings::IniFormat);
    const QString fakeHash = QString(64, QLatin1Char('a'));
    settings.setValue(QStringLiteral("gif/starred"),
                      QStringLiteral("[{\"provider\":\"local\",\"id\":\"%1\","
                                     "\"w\":10,\"h\":10,\"bytes\":10}]")
                          .arg(fakeHash));
    settings.sync();

    GifStarredStore store;
    store.openFor(dir.path());
    QCOMPARE(store.count(), 0);
}

void GifStarredStoreTest::sessionMediaKeyMappingDrivesUnstarByMediaKey()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.starBytes(QStringLiteral("m1"), makeGif(50, 50));
    QVERIFY(store.isStarredThisSession(QStringLiteral("m1")));
    QVERIFY(!store.isStarredThisSession(QStringLiteral("unrelated")));

    store.unstarByMediaKey(QStringLiteral("unrelated"));
    QCOMPARE(store.count(), 1);

    store.unstarByMediaKey(QStringLiteral("m1"));
    QCOMPARE(store.count(), 0);
    QVERIFY(!store.isStarredThisSession(QStringLiteral("m1")));
}

void GifStarredStoreTest::reStarringAfterFileWasManuallyDeletedRewritesIt()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    const QByteArray gif = makeGif(20, 20);
    store.starBytes(QStringLiteral("mk1"), gif);
    const QString hash = store.model()->get(0).value(QStringLiteral("gifId")).toString();
    const QString path = dir.path() + QLatin1Char('/') + hash + QStringLiteral(".gif");
    QVERIFY(QFile::remove(path));
    QVERIFY(store.model()->hasHash(hash));

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk2"), gif);
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(1).toBool());
    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(store.readBytes(hash), gif);
}

void GifStarredStoreTest::openForSweepsOrphanedFilesNotInIndex()
{
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
    QVERIFY(!QFileInfo::exists(orphanPath));
    QVERIFY(QFileInfo::exists(
        dir.path() + QLatin1Char('/') + keptHash + QStringLiteral(".gif")));
}

void GifStarredStoreTest::oversizedFileOnDiskRejectedByReadBytes()
{
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
    QCOMPARE(store.count(), 0);
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
    QTemporaryDir dir;
    const QString starredDir = dir.path() + QStringLiteral("/starred-gifs");
    GifStarredStore store;
    store.openFor(starredDir);
    QVERIFY(store.isOpen());
    QVERIFY(!QDir(starredDir).exists());

    store.starBytes(QStringLiteral("mk"), makeGif(10, 10));
    QVERIFY(QDir(starredDir).exists());
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

void GifStarredStoreTest::sessionMapIsAlreadyClearedWhenCountChangedFires()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());

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
    QCOMPARE(unstarSpy.count(), 0);

    store.unstar(hash);
    QCOMPARE(unstarSpy.count(), 1);
    QCOMPARE(unstarSpy.at(0).at(0).toString(), hash);
}

void GifStarredStoreTest::categoryMessagesAreTranslatedNeverRawTokens()
{
    QTemporaryDir dir;
    GifStarredStore store;
    store.openFor(dir.path());
    store.setCapsForTest(1,64LL * 1024 * 1024);
    store.starBytes(QStringLiteral("mk1"), makeGif(10, 10, "a"));

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk2"), makeGif(10, 10, "b"));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cap_items"));
    const QString message = finished.at(0).at(3).toString();
    QVERIFY(!message.isEmpty());
    QVERIFY(!message.contains(QStringLiteral("cap_items")));
    QVERIFY(message.contains(QStringLiteral("1")));
}

void GifStarredStoreTest::legacyIndexEntryWithNoExtFieldLoadsAndSendsAsGif()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path()));
    const QByteArray gif = makeGif(12, 12);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(gif, QCryptographicHash::Sha256).toHex());

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
    QCOMPARE(store.count(), 1);
    QVERIFY(store.model()->hasHash(hash));
    QVERIFY(!store.source(hash).isEmpty());
    QCOMPARE(store.readBytes(hash), gif);
    QCOMPARE(store.sourceExt(hash), QStringLiteral("gif"));
}

void GifStarredStoreTest::hostileExtInIndexNeverReachesAPath()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path()));
    const QByteArray gif = makeGif(12, 12);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(gif, QCryptographicHash::Sha256).toHex());

    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString decoy = outside.path() + QStringLiteral("/decoy.txt");
    {
        QFile f(decoy);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("do not delete");
    }
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
    QCOMPARE(store.count(), 1);
    QCOMPARE(store.sourceExt(hash), QStringLiteral("gif"));
    QVERIFY(store.source(hash).contains(hash + QStringLiteral(".gif")));
    QCOMPARE(store.readBytes(hash), gif);
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
    for (const QString &wrongExt : { QStringLiteral("jpg"), QStringLiteral("webp") }) {
        QVERIFY(!QFileInfo::exists(
            dir.path() + QLatin1Char('/') + hash + QLatin1Char('.') + wrongExt));
    }
}

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
    store.setCapsForTest(1,64LL * 1024 * 1024);
    store.starBytes(QStringLiteral("mk1"), makePng(10, 10));

    QSignalSpy finished(&store, &GifStarredStore::starFinished);
    store.starBytes(QStringLiteral("mk2"), makeJpeg(10, 10));
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(), QStringLiteral("cap_items"));
    QCOMPARE(store.count(), 1);
}

QTEST_MAIN(GifStarredStoreTest)
#include "GifStarredStoreTest.moc"
