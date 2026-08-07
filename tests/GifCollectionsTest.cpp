// v0.6.1: local GIF Favorites and Recents. Exercises persistence, dedup,
// ordering, bounded history, the record-only-on-send rule, recording toggle,
// clear actions, corrupted-store recovery, and the safety rule that no
// non-provider / non-https URL and no Matrix identifier is ever stored.

#include "gif/GifFavoritesModel.h"
#include "gif/GifRecentModel.h"
#include "gif/GifResultModel.h"
#include "gif/GifStarredModel.h"

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

// A local-starred row exactly as GifStarredStore builds it: provider
// "local", id a content-hash-shaped string, no URLs at all (see
// GifStarredStore's header — the actual playback source is re-derived from
// the hash, never trusted from a persisted URL).
gif::GifResult makeLocal(const QString &hash, qint64 bytes = 1234)
{
    gif::GifResult r;
    r.provider = QStringLiteral("local");
    r.id = hash;
    r.gifWidth = 64;
    r.gifHeight = 48;
    r.gifBytes = bytes;
    return r;
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

    // v0.6.6: GifStarredModel — the thin GifStoredModel sibling
    // GifStarredStore persists local-starred rows through.
    void starredInsertDedupsByHashAndOrdersNewestFirst();
    void starredPersistsAcrossReloadWithNoUrlFields();
    void starredUnstarRemovesEntry();
    void starredTotalBytesSumsRows();
    void starredNoSensitiveFieldsPersisted();
    void reopenReplacesRowsAndCanGoStorageless();
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

void GifCollectionsTest::starredInsertDedupsByHashAndOrdersNewestFirst()
{
    GifStarredModel starred(store);
    const QString h1 = QString(64, QLatin1Char('1'));
    const QString h2 = QString(64, QLatin1Char('2'));
    starred.insertLocal(makeLocal(h1));
    starred.insertLocal(makeLocal(h2));
    QCOMPARE(starred.count(), 2);
    QCOMPARE(starred.get(0).value(QStringLiteral("gifId")).toString(), h2); // newest first
    QVERIFY(starred.hasHash(h1));
    QVERIFY(starred.hasHash(h2));

    // Re-inserting the SAME hash (re-starring identical content) moves it
    // to the front instead of duplicating.
    starred.insertLocal(makeLocal(h1));
    QCOMPARE(starred.count(), 2);
    QCOMPARE(starred.get(0).value(QStringLiteral("gifId")).toString(), h1);
}

void GifCollectionsTest::starredPersistsAcrossReloadWithNoUrlFields()
{
    const QString h = QString(64, QLatin1Char('a'));
    {
        GifStarredModel starred(store);
        starred.insertLocal(makeLocal(h, 5000));
        QCOMPARE(starred.count(), 1);
    }
    store->sync();
    GifStarredModel reloaded(store);
    QCOMPARE(reloaded.count(), 1);
    const QVariantMap row = reloaded.get(0);
    QCOMPARE(row.value(QStringLiteral("provider")).toString(),
             QStringLiteral("local"));
    QCOMPARE(row.value(QStringLiteral("gifId")).toString(), h);
    QCOMPARE(row.value(QStringLiteral("previewUrl")).toString(), QString());
    QCOMPARE(row.value(QStringLiteral("gifUrl")).toString(), QString());
}

void GifCollectionsTest::starredUnstarRemovesEntry()
{
    GifStarredModel starred(store);
    const QString h = QString(64, QLatin1Char('b'));
    starred.insertLocal(makeLocal(h));
    QCOMPARE(starred.count(), 1);
    starred.unstar(h);
    QCOMPARE(starred.count(), 0);
    QVERIFY(!starred.hasHash(h));
}

void GifCollectionsTest::starredTotalBytesSumsRows()
{
    GifStarredModel starred(store);
    starred.insertLocal(makeLocal(QString(64, QLatin1Char('c')), 1000));
    starred.insertLocal(makeLocal(QString(64, QLatin1Char('d')), 2500));
    QCOMPARE(starred.totalBytes(), qint64(3500));
}

void GifCollectionsTest::starredNoSensitiveFieldsPersisted()
{
    GifStarredModel starred(store);
    starred.insertLocal(makeLocal(QString(64, QLatin1Char('e'))));
    store->sync();
    const QString raw = store->value(QStringLiteral("gif/starred")).toString();
    QVERIFY(raw.contains(QStringLiteral("provider")));
    for (const char *forbidden : { "roomId", "eventId", "threadRootId",
                                   "userId", "!room", "$event", "@user",
                                   "access_token", "mediaKey", "sender" }) {
        QVERIFY2(!raw.contains(QLatin1String(forbidden)), forbidden);
    }
}

void GifCollectionsTest::reopenReplacesRowsAndCanGoStorageless()
{
    GifStarredModel starred(store);
    starred.insertLocal(makeLocal(QString(64, QLatin1Char('f'))));
    QCOMPARE(starred.count(), 1);

    // Re-point at "no backing store" (closed) — every row drops, and
    // nothing is written back to the OLD settings object (still holding
    // the account-A row untouched on disk).
    starred.reopen(nullptr);
    QCOMPARE(starred.count(), 0);

    // Re-point at a DIFFERENT settings object (a different account
    // directory) and load whatever it holds.
    QSettings other(QStringLiteral("/tmp/lightning-gif-collections-test-b.ini"),
                    QSettings::IniFormat);
    other.clear();
    starred.reopen(&other);
    QCOMPARE(starred.count(), 0); // fresh store, nothing in it yet
    starred.insertLocal(makeLocal(QString(64, QLatin1Char('g'))));
    QCOMPARE(starred.count(), 1);
    other.sync();

    // The original settings object's "gif/starred" key is untouched by any
    // of the above — reopen() never persists into the store it just left.
    store->sync();
    GifStarredModel original(store);
    QCOMPARE(original.count(), 1);
    QCOMPARE(original.get(0).value(QStringLiteral("gifId")).toString(),
             QString(64, QLatin1Char('f')));
    other.clear();
}

QTEST_MAIN(GifCollectionsTest)
#include "GifCollectionsTest.moc"
