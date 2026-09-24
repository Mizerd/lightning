// Local GIF favorites and recents: persistence, dedup, ordering, bounded
// history, recording only on send, the recording toggle, clear actions,
// corrupted-store recovery, and that no non-provider or non-https URL and no
// Matrix identifier is ever stored.

#include "gif/GifFavoritesModel.h"
#include "gif/GifRecentModel.h"
#include "gif/GifResultModel.h"
#include "gif/GifSavedModel.h"
#include "gif/GifStarredModel.h"

#include <QSettings>
#include <QTemporaryDir>
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

// A local saved row as GifStarredStore builds it: provider "local", a
// content-hash id and no URLs (playback is re-derived from the hash, never a
// persisted URL).
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

    // A per-run QTemporaryDir, so overlapping CTest runs of the two build trees
    // cannot collide.
    QTemporaryDir tempDir;
    QSettings *store = nullptr;

    QString storePath(const QString &name) const
    { return tempDir.filePath(name + QStringLiteral(".ini")); }

private Q_SLOTS:
    void initTestCase() { QVERIFY(tempDir.isValid()); }

    void init()
    {
        // A fresh store per test (INI file under this run's temp dir).
        store = new QSettings(storePath(QStringLiteral("collections")),
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
    void malformedStoreShapesAreAllSurvivable();
    void duplicateStoredEntriesLoadOnce();
    void rejectsUnsafeStoredUrl();
    void noSensitiveFieldsPersisted();

    // GifStarredModel: the GifStoredModel sibling GifStarredStore persists
    // through.
    void starredInsertDedupsByHashAndOrdersNewestFirst();
    void starredPersistsAcrossReloadWithNoUrlFields();
    void starredUnstarRemovesEntry();
    void starredTotalBytesSumsRows();
    void starredNoSensitiveFieldsPersisted();
    void reopenReplacesRowsAndCanGoStorageless();

    // GifSavedModel: the one visible "Saved" list, merging two stores that
    // stay separate underneath.
    void savedListsLocalRowsFirstThenProviderRows();
    void savedGetIsBoundsCheckedAcrossTheGroupBoundary();
    void savedTracksBothSourcesLive();
    void savedForwardsRolesCorrectlyForBothGroups();
    void savedSurvivesASourceReopen();
    void favoriteRoleIsAConstantAndNotASavedStateOracle();

    // The persisted "ext" field (gif::GifResult::localExt), written only when
    // non-empty so favorites/recents entries round-trip unchanged.
    void localExtRoundTripsThroughJsonWhenSet();
    void missingExtFieldDefaultsToEmptyNeverCrashes();
    void favoritesEntryNeverGainsAnExtKey();
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

// Malformed store shapes that partial writes or hand edits produce, beyond
// invalid JSON, must all be survivable and leave the model usable. (A reported
// reopen crash was never reproduced; this does not claim its cause.)
void GifCollectionsTest::malformedStoreShapesAreAllSurvivable()
{
    const QString key = QStringLiteral("gif/favorites");
    for (const QString &payload : {
             // Valid JSON, wrong top-level type.
             QStringLiteral("{\"provider\":\"giphy\"}"),
             QStringLiteral("\"a string\""),
             QStringLiteral("42"),
             QStringLiteral("null"),
             // An array of things that are not entries.
             QStringLiteral("[1,2,3]"),
             QStringLiteral("[null,null]"),
             QStringLiteral("[[],[]]"),
             QStringLiteral("[\"x\"]"),
             // Entries missing the identity fields.
             QStringLiteral("[{}]"),
             QStringLiteral("[{\"provider\":\"giphy\"}]"),
             QStringLiteral("[{\"id\":\"only\"}]"),
             // Wrong field types, which .toString()/.toInt() would coerce; the
             // entry must still fail validation.
             QStringLiteral("[{\"provider\":5,\"id\":true,\"gifUrl\":[]}]"),
             // Truncated mid-write, the realistic corruption.
             QStringLiteral("[{\"provider\":\"giphy\",\"id\":\"a\","),
             QStringLiteral("["),
             QStringLiteral(""),
         }) {
        store->setValue(key, payload);
        store->sync();
        GifFavoritesModel fav(store);   // must not crash
        QCOMPARE(fav.count(), 0);
        // ...and the model is still usable afterwards.
        QVERIFY2(fav.toggle(toMap(make("giphy", "ok"))), qPrintable(payload));
        QCOMPARE(fav.count(), 1);
        store->remove(key);
    }
}

// A store holding the same GIF twice loads it once; a half-completed write is
// the likely source, and two tiles would show while isFavorite() stayed right.
void GifCollectionsTest::duplicateStoredEntriesLoadOnce()
{
    store->setValue(
        QStringLiteral("gif/favorites"),
        QStringLiteral("[{\"provider\":\"giphy\",\"id\":\"dup\","
                       "\"gifUrl\":\"https://media.giphy.com/a.gif\","
                       "\"previewUrl\":\"https://media.giphy.com/a.gif\","
                       "\"stillUrl\":\"https://media.giphy.com/a.gif\"},"
                       "{\"provider\":\"giphy\",\"id\":\"dup\","
                       "\"gifUrl\":\"https://media.giphy.com/a.gif\","
                       "\"previewUrl\":\"https://media.giphy.com/a.gif\","
                       "\"stillUrl\":\"https://media.giphy.com/a.gif\"}]"));
    store->sync();
    GifFavoritesModel fav(store);
    QCOMPARE(fav.count(), 1);
    QVERIFY(fav.isFavorite(QStringLiteral("giphy"), QStringLiteral("dup")));
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
    // Provider identity and safe media fields only, never Matrix context.
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

    // Re-inserting the same hash moves it to the front instead of duplicating.
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

    // Repoint at no backing store (closed): every row drops, and nothing is
    // written back to the old settings object.
    starred.reopen(nullptr);
    QCOMPARE(starred.count(), 0);

    // Repoint at another account's settings and load what it holds.
    QSettings other(storePath(QStringLiteral("account-b")),
                    QSettings::IniFormat);
    other.clear();
    starred.reopen(&other);
    QCOMPARE(starred.count(), 0); // fresh store, nothing in it yet
    starred.insertLocal(makeLocal(QString(64, QLatin1Char('g'))));
    QCOMPARE(starred.count(), 1);
    other.sync();

    // reopen() never persisted into the store it left.
    store->sync();
    GifStarredModel original(store);
    QCOMPARE(original.count(), 1);
    QCOMPARE(original.get(0).value(QStringLiteral("gifId")).toString(),
             QString(64, QLatin1Char('f')));
    other.clear();
}

// ---- the merged Saved list ----

void GifCollectionsTest::savedListsLocalRowsFirstThenProviderRows()
{
    GifStarredModel local(store);
    GifFavoritesModel provider(store);
    GifSavedModel saved(&local, &provider);
    QCOMPARE(saved.count(), 0);

    QVERIFY(provider.toggle(toMap(make("giphy", "p1"))));
    QVERIFY(provider.toggle(toMap(make("klipy", "p2"))));
    const QString h1 = QString(64, QLatin1Char('1'));
    const QString h2 = QString(64, QLatin1Char('2'));
    local.insertLocal(makeLocal(h1));
    local.insertLocal(makeLocal(h2));

    // Grouped by kind, each newest-first: locals, then provider bookmarks. Not
    // a chronological interleave: gif::GifResult has no saved-at timestamp.
    QCOMPARE(saved.count(), 4);
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(), h2);
    QCOMPARE(saved.get(1).value(QStringLiteral("gifId")).toString(), h1);
    QCOMPARE(saved.get(2).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p2"));
    QCOMPARE(saved.get(3).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
    // Provider identity survives the merge: it routes a star press to the
    // right store and feeds the tile's source tag.
    QCOMPARE(saved.get(0).value(QStringLiteral("provider")).toString(),
             QStringLiteral("local"));
    QCOMPARE(saved.get(2).value(QStringLiteral("provider")).toString(),
             QStringLiteral("klipy"));
}

void GifCollectionsTest::savedGetIsBoundsCheckedAcrossTheGroupBoundary()
{
    GifStarredModel local(store);
    GifFavoritesModel provider(store);
    GifSavedModel saved(&local, &provider);
    local.insertLocal(makeLocal(QString(64, QLatin1Char('3'))));
    QVERIFY(provider.toggle(toMap(make("giphy", "p1"))));
    QCOMPARE(saved.count(), 2);

    // An out-of-range row answers an empty map, never a neighbour:
    // GifPicker.qml's choose() drops a result without provider/gifId, so a
    // stale keyboard row sends nothing.
    QVERIFY(saved.get(2).isEmpty());
    QVERIFY(saved.get(99).isEmpty());
    QVERIFY(saved.get(-1).isEmpty());
}

void GifCollectionsTest::savedTracksBothSourcesLive()
{
    GifStarredModel local(store);
    GifFavoritesModel provider(store);
    GifSavedModel saved(&local, &provider);
    QSignalSpy counted(&saved, &GifSavedModel::countChanged);

    QVERIFY(provider.toggle(toMap(make("giphy", "p1"))));
    QCOMPARE(saved.count(), 1);
    const QString h = QString(64, QLatin1Char('4'));
    local.insertLocal(makeLocal(h));
    QCOMPARE(saved.count(), 2);
    QVERIFY(counted.count() >= 2);

    // Unsaving through either store drops the row from the list.
    local.unstar(h);
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
    provider.unfavorite(QStringLiteral("giphy"), QStringLiteral("p1"));
    QCOMPARE(saved.count(), 0);
}

void GifCollectionsTest::savedForwardsRolesCorrectlyForBothGroups()
{
    GifStarredModel local(store);
    GifFavoritesModel provider(store);
    GifSavedModel saved(&local, &provider);
    local.insertLocal(makeLocal(QString(64, QLatin1Char('5')), 4242));
    QVERIFY(provider.toggle(toMap(make("giphy", "p1"))));
    QCOMPARE(saved.rowCount(), 2);

    // QConcatenateTablesProxyModel forwards numeric roles unremapped, which is
    // only correct because both sources share the GifResultModel role table.
    QCOMPARE(local.roleNames(), provider.roleNames());
    // The proxy's table is a superset (Qt's default item roles are added);
    // every GIF role must keep its number->name pairing. Both hashes are held
    // by value: iterating across two temporaries is undefined.
    const QHash<int, QByteArray> proxyRoles = saved.roleNames();
    const QHash<int, QByteArray> sourceRoles = local.roleNames();
    for (auto it = sourceRoles.cbegin(); it != sourceRoles.cend(); ++it) {
        QVERIFY2(proxyRoles.value(it.key()) == it.value(),
                 qPrintable(QStringLiteral("role %1 (%2) changed meaning")
                                .arg(it.key())
                                .arg(QString::fromUtf8(it.value()))));
    }

    const int providerRole = GifResultModel::ProviderRole;
    const int bytesRole = GifResultModel::BytesRole;
    QCOMPARE(saved.data(saved.index(0, 0), providerRole).toString(),
             QStringLiteral("local"));
    QCOMPARE(saved.data(saved.index(0, 0), bytesRole).toLongLong(),
             qint64(4242));
    QCOMPARE(saved.data(saved.index(1, 0), providerRole).toString(),
             QStringLiteral("giphy"));
}

// GifStarredStore::openFor()/close() repoint the long-lived GifStarredModel
// via reopen(), emitting two reset pairs; the proxy must relay them or the
// Saved tab keeps the previous account's rows.
void GifCollectionsTest::savedSurvivesASourceReopen()
{
    GifStarredModel local(store);
    GifFavoritesModel provider(store);
    GifSavedModel saved(&local, &provider);

    local.insertLocal(makeLocal(QString(64, QLatin1Char('7'))));
    QVERIFY(provider.toggle(toMap(make("giphy", "p1"))));
    QCOMPARE(saved.count(), 2);
    QCOMPARE(saved.get(0).value(QStringLiteral("provider")).toString(),
             QStringLiteral("local"));

    QSignalSpy reset(&saved, &QAbstractItemModel::modelReset);
    QSignalSpy counted(&saved, &GifSavedModel::countChanged);

    // Sign-out: the local group goes storageless; the proxy drops exactly that
    // group and keeps the provider group.
    local.reopen(nullptr);
    QVERIFY(reset.count() >= 1);
    QVERIFY(counted.count() >= 1);
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.get(0).value(QStringLiteral("provider")).toString(),
             QStringLiteral("giphy"));
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
    QVERIFY(saved.get(1).isEmpty());

    // Another account's directory: its rows appear ahead of the provider group
    // and none of the previous account's survive.
    QSettings other(storePath(QStringLiteral("account-c")),
                    QSettings::IniFormat);
    other.clear();
    local.reopen(&other);
    const QString h = QString(64, QLatin1Char('8'));
    local.insertLocal(makeLocal(h));
    QCOMPARE(saved.count(), 2);
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(), h);
    QCOMPARE(saved.get(1).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
    other.clear();
}

// GifStoredModel's FavoriteRole is a constant `true` ("stored == favorited"),
// which is wrong for recents. It must not be used as a saved-state oracle;
// the collection lookup is what GifPicker.qml's isSaved() asks.
void GifCollectionsTest::favoriteRoleIsAConstantAndNotASavedStateOracle()
{
    GifRecentModel recent(store);
    GifFavoritesModel favorites(store);

    recent.recordSent(make("giphy", "sent1"));
    QCOMPARE(recent.count(), 1);
    // The role claims "favorite" for a GIF that was only sent.
    QCOMPARE(recent.data(recent.index(0, 0),
                         GifResultModel::FavoriteRole).toBool(),
             true);
    // The collection answers truthfully.
    QVERIFY(!favorites.isFavorite(QStringLiteral("giphy"),
                                  QStringLiteral("sent1")));

    // ...and says yes once it really is saved.
    QVERIFY(favorites.toggle(toMap(make("giphy", "sent1"))));
    QVERIFY(favorites.isFavorite(QStringLiteral("giphy"),
                                 QStringLiteral("sent1")));
    favorites.unfavorite(QStringLiteral("giphy"), QStringLiteral("sent1"));
    QVERIFY(!favorites.isFavorite(QStringLiteral("giphy"),
                                  QStringLiteral("sent1")));
}

// A local row's format survives a reload: GifStoredModel's JSON round trip
// (GifStarredStoreTest covers the store that sets localExt).
void GifCollectionsTest::localExtRoundTripsThroughJsonWhenSet()
{
    const QString h = QString(64, QLatin1Char('1'));
    {
        GifStarredModel starred(store);
        gif::GifResult r = makeLocal(h, 2000);
        r.localExt = QStringLiteral("png");
        starred.insertLocal(r);
        QCOMPARE(starred.count(), 1);
    }
    store->sync();
    const QString raw = store->value(QStringLiteral("gif/starred")).toString();
    QVERIFY(raw.contains(QStringLiteral("\"ext\":\"png\"")));

    GifStarredModel reloaded(store);
    QCOMPARE(reloaded.count(), 1);
    QCOMPARE(reloaded.resultAt(0).localExt, QStringLiteral("png"));
}

// A row with no "ext" key (older rows, and every favorites/recents row) loads
// with localExt empty. Mapping empty to "gif" is GifStarredStore's
// convention, applied where it needs a file suffix, not in the model layer.
void GifCollectionsTest::missingExtFieldDefaultsToEmptyNeverCrashes()
{
    const QString h = QString(64, QLatin1Char('2'));
    store->setValue(QStringLiteral("gif/starred"),
                    QStringLiteral("[{\"provider\":\"local\",\"id\":\"%1\","
                                   "\"w\":10,\"h\":10,\"bytes\":10}]")
                        .arg(h));
    store->sync();
    GifStarredModel starred(store); // must not crash
    QCOMPARE(starred.count(), 1);
    QCOMPARE(starred.resultAt(0).localExt, QString());
}

// A favorites row never sets localExt, so toJson() writes no "ext" key and
// its persisted shape is unchanged.
void GifCollectionsTest::favoritesEntryNeverGainsAnExtKey()
{
    GifFavoritesModel fav(store);
    fav.toggle(toMap(make("giphy", "a")));
    store->sync();
    const QString raw = store->value(QStringLiteral("gif/favorites")).toString();
    QVERIFY(!raw.contains(QStringLiteral("\"ext\"")));
}

QTEST_MAIN(GifCollectionsTest)
#include "GifCollectionsTest.moc"
