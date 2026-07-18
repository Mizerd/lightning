// v0.6.1: local GIF Favorites and Recents. Exercises persistence, dedup,
// ordering, bounded history, the record-only-on-send rule, recording toggle,
// clear actions, corrupted-store recovery, and the safety rule that no
// non-provider / non-https URL and no Matrix identifier is ever stored.

#include "gif/GifFavoritesModel.h"
#include "gif/GifRecentModel.h"
#include "gif/GifResultModel.h"

#include <QSettings>
#include <QtTest/QtTest>

namespace {

gif::GifResult make(const QString &provider, const QString &id)
{
    gif::GifResult r;
    r.provider = provider;
    r.id = id;
    r.title = QStringLiteral("t") + id;
    r.rating = QStringLiteral("g");
    r.previewUrl = QStringLiteral("https://media.giphy.com/media/%1/200w.gif").arg(id);
    r.stillUrl = QStringLiteral("https://media.giphy.com/media/%1/200w_s.gif").arg(id);
    r.gifUrl = QStringLiteral("https://media.giphy.com/media/%1/giphy.gif").arg(id);
    r.gifWidth = 200;
    r.gifHeight = 200;
    return r;
}

QVariantMap toMap(const gif::GifResult &r)
{
    QVariantMap m;
    m.insert(QStringLiteral("provider"), r.provider);
    m.insert(QStringLiteral("gifId"), r.id);
    m.insert(QStringLiteral("title"), r.title);
    m.insert(QStringLiteral("rating"), r.rating);
    m.insert(QStringLiteral("previewUrl"), r.previewUrl);
    m.insert(QStringLiteral("stillUrl"), r.stillUrl);
    m.insert(QStringLiteral("gifUrl"), r.gifUrl);
    m.insert(QStringLiteral("gifWidth"), r.gifWidth);
    m.insert(QStringLiteral("gifHeight"), r.gifHeight);
    return m;
}

} // namespace

class GifCollectionsTest : public QObject
{
    Q_OBJECT

    QSettings *store = nullptr;

private Q_SLOTS:
    void init()
    {
        // A fresh in-memory-ish store per test (INI temp file).
        store = new QSettings(QStringLiteral("/tmp/lightning-gif-collections-test.ini"),
                              QSettings::IniFormat);
        store->clear();
    }
    void cleanup()
    {
        store->clear();
        delete store;
        store = nullptr;
    }

    void favoriteToggleAndDedup();
    void favoritesPersistAcrossReload();
    void favoritesRetainProviderIdentity();
    void recentAddedNewestFirstAndDedup();
    void recentBoundedToCap();
    void recentRecordingDisabled();
    void clearActions();
    void corruptedStoreRecovers();
    void rejectsUnsafeStoredUrl();
    void noSensitiveFieldsPersisted();
};

void GifCollectionsTest::favoriteToggleAndDedup()
{
    GifFavoritesModel fav(store);
    QVERIFY(fav.toggle(toMap(make("giphy", "a"))));   // added
    QCOMPARE(fav.count(), 1);
    QVERIFY(fav.isFavorite(QStringLiteral("giphy"), QStringLiteral("a")));
    QVERIFY(!fav.toggle(toMap(make("giphy", "a"))));  // toggled off
    QCOMPARE(fav.count(), 0);
    // A second add re-favorites without ever duplicating.
    QVERIFY(fav.toggle(toMap(make("giphy", "a"))));
    QVERIFY(fav.toggle(toMap(make("giphy", "b"))));
    QCOMPARE(fav.count(), 2);
    fav.unfavorite(QStringLiteral("giphy"), QStringLiteral("a"));
    QCOMPARE(fav.count(), 1);
}

void GifCollectionsTest::favoritesPersistAcrossReload()
{
    {
        GifFavoritesModel fav(store);
        fav.toggle(toMap(make("giphy", "x")));
        fav.toggle(toMap(make("klipy", "y")));
        QCOMPARE(fav.count(), 2);
    }
    store->sync();
    GifFavoritesModel reloaded(store);
    QCOMPARE(reloaded.count(), 2);
    QVERIFY(reloaded.isFavorite(QStringLiteral("klipy"), QStringLiteral("y")));
}

void GifCollectionsTest::favoritesRetainProviderIdentity()
{
    GifFavoritesModel fav(store);
    fav.toggle(toMap(make("giphy", "1")));
    fav.toggle(toMap(make("klipy", "1"))); // same id, different provider
    QCOMPARE(fav.count(), 2);              // provider is part of identity
    QVERIFY(fav.isFavorite(QStringLiteral("giphy"), QStringLiteral("1")));
    QVERIFY(fav.isFavorite(QStringLiteral("klipy"), QStringLiteral("1")));
}

void GifCollectionsTest::recentAddedNewestFirstAndDedup()
{
    GifRecentModel recent(store);
    recent.recordSent(make("giphy", "a"));
    recent.recordSent(make("giphy", "b"));
    QCOMPARE(recent.count(), 2);
    QCOMPARE(recent.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("b")); // newest first
    // Re-sending "a" moves it to the front, no duplicate.
    recent.recordSent(make("giphy", "a"));
    QCOMPARE(recent.count(), 2);
    QCOMPARE(recent.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("a"));
}

void GifCollectionsTest::recentBoundedToCap()
{
    GifRecentModel recent(store);
    for (int i = 0; i < 80; ++i)
        recent.recordSent(make("giphy", QString::number(i)));
    QVERIFY(recent.count() <= 60);        // kMaxRecent
    // The newest is retained, the oldest evicted.
    QCOMPARE(recent.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("79"));
}

void GifCollectionsTest::recentRecordingDisabled()
{
    GifRecentModel recent(store);
    recent.recordSent(make("giphy", "keep"));
    recent.setRecordingEnabled(false);
    recent.recordSent(make("giphy", "dropped"));
    QCOMPARE(recent.count(), 1);          // nothing added while disabled
    QVERIFY(!recent.contains(QStringLiteral("giphy"), QStringLiteral("dropped")));
}

void GifCollectionsTest::clearActions()
{
    GifFavoritesModel fav(store);
    fav.toggle(toMap(make("giphy", "a")));
    fav.toggle(toMap(make("giphy", "b")));
    fav.clearAll();
    QCOMPARE(fav.count(), 0);
    store->sync();
    GifFavoritesModel reloaded(store);
    QCOMPARE(reloaded.count(), 0);        // clear persisted
}

void GifCollectionsTest::corruptedStoreRecovers()
{
    store->setValue(QStringLiteral("gif/favorites"),
                    QStringLiteral("{not valid json"));
    store->sync();
    GifFavoritesModel fav(store);         // must not crash
    QCOMPARE(fav.count(), 0);
    QVERIFY(fav.toggle(toMap(make("giphy", "ok")))); // still usable
}

void GifCollectionsTest::rejectsUnsafeStoredUrl()
{
    // A persisted entry whose gifUrl is not https must not load back.
    store->setValue(
        QStringLiteral("gif/favorites"),
        QStringLiteral("[{\"provider\":\"giphy\",\"id\":\"bad\","
                       "\"gifUrl\":\"http://evil/x.gif\"}]"));
    store->sync();
    GifFavoritesModel fav(store);
    QCOMPARE(fav.count(), 0);
}

void GifCollectionsTest::noSensitiveFieldsPersisted()
{
    GifFavoritesModel fav(store);
    fav.toggle(toMap(make("giphy", "a")));
    store->sync();
    const QString raw = store->value(QStringLiteral("gif/favorites")).toString();
    // Provider identity + safe media fields only — never Matrix context.
    QVERIFY(raw.contains(QStringLiteral("provider")));
    QVERIFY(raw.contains(QStringLiteral("gifUrl")));
    for (const char *forbidden : { "roomId", "eventId", "threadRootId",
                                   "userId", "!room", "$event", "@user",
                                   "access_token", "query" }) {
        QVERIFY2(!raw.contains(QLatin1String(forbidden)),
                 forbidden);
    }
}

QTEST_MAIN(GifCollectionsTest)
#include "GifCollectionsTest.moc"
