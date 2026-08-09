// v0.6.1: local GIF Favorites and Recents. Exercises persistence, dedup,
// ordering, bounded history, the record-only-on-send rule, recording toggle,
// clear actions, corrupted-store recovery, and the safety rule that no
// non-provider / non-https URL and no Matrix identifier is ever stored.

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

    // v0.6.7 review (N6): a per-run QTemporaryDir, not a fixed /tmp path. The
    // hard-coded name collided if two CTest runs ever overlapped (the trees
    // are built and tested separately, so that is reachable), and a collision
    // here looks like a data bug rather than a harness one.
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

    // v0.6.7: GifSavedModel — the single user-visible "Saved" list, a
    // presentation merge over the two stores that stay separate underneath.
    void savedListsLocalRowsFirstThenProviderRows();
    void savedGetIsBoundsCheckedAcrossTheGroupBoundary();
    void savedTracksBothSourcesLive();
    void savedForwardsRolesCorrectlyForBothGroups();
    void savedSurvivesASourceReopen();
    void favoriteRoleIsAConstantAndNotASavedStateOracle();
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
    QSettings other(storePath(QStringLiteral("account-b")),
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

// ── v0.6.7: the merged Saved list ───────────────────────────────────────

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

    // Grouped by kind, each group newest-first: locals first, then provider
    // bookmarks. NOT a chronological interleave — gif::GifResult carries no
    // saved-at timestamp, so claiming one would be fiction.
    QCOMPARE(saved.count(), 4);
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(), h2);
    QCOMPARE(saved.get(1).value(QStringLiteral("gifId")).toString(), h1);
    QCOMPARE(saved.get(2).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p2"));
    QCOMPARE(saved.get(3).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
    // Provider identity survives the merge — it is what routes a star press
    // back to the right store, and what the tile's source tag displays.
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

    // An out-of-range row must answer an EMPTY map, never a neighbouring row.
    // GifPicker.qml's choose() drops a result with no provider/gifId, so an
    // empty map is what makes a stale keyboard row send nothing rather than
    // send the wrong GIF — the whole reason the offset arithmetic here is
    // bounds-checked on the provider side too, not only on the local side.
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

    // Unsaving through EITHER store drops the row from the one visible list.
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

    // QConcatenateTablesProxyModel forwards the numeric role AS GIVEN to
    // whichever source a row maps to, with no per-source remapping by name.
    // That is only correct because both sources answer the IDENTICAL
    // GifResultModel role table — assert that directly rather than trusting
    // it, because two sources with similarly-named but differently-numbered
    // roles would silently cross-wire fields with no visible error.
    QCOMPARE(local.roleNames(), provider.roleNames());
    // The proxy's own table is a SUPERSET — QConcatenateTablesProxyModel adds
    // Qt's default item roles (display/decoration/edit/...) on top. What has
    // to hold is that every GIF role keeps its exact number->name pairing
    // through the merge, which is what makes the un-remapped forwarding above
    // correct.
    // Both hashes held by value: roleNames() returns a temporary, and
    // iterating from one temporary's begin() to another's end() is undefined.
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

// v0.6.7 review (M2): the merged view's riskiest path had no coverage —
// GifStarredStore::openFor()/close() repoint the SAME long-lived
// GifStarredModel at a different account's file through reopen(), which emits
// two back-to-back reset pairs. If the proxy did not relay those, the Saved
// tab would keep rendering the PREVIOUS account's rows after a switch.
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

    // Sign-out / account close: the local group goes storageless. The proxy
    // must drop exactly that group and keep the provider group.
    local.reopen(nullptr);
    QVERIFY(reset.count() >= 1);
    QVERIFY(counted.count() >= 1);
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.get(0).value(QStringLiteral("provider")).toString(),
             QStringLiteral("giphy"));
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
    QVERIFY(saved.get(1).isEmpty());

    // Switching to another account's directory: its rows appear, ahead of the
    // provider group again, and none of the previous account's survive.
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

// v0.6.7 review (H1): GifStoredModel answers FavoriteRole with a constant
// `true` — "stored == favorited". That is honest for favorites and for
// locally-saved rows, and a LIE for recents, whose rows are merely recently
// sent. The picker read that role to drive its star, so every Recent tile
// rendered as saved, announced "Remove from saved GIFs", and then INSERTED on
// activation. This pins the trap itself so the next reader cannot mistake the
// role for a saved-state oracle, and pins the store lookup that replaced it.
void GifCollectionsTest::favoriteRoleIsAConstantAndNotASavedStateOracle()
{
    GifRecentModel recent(store);
    GifFavoritesModel favorites(store);

    recent.recordSent(make("giphy", "sent1"));
    QCOMPARE(recent.count(), 1);
    // The role claims "favorite" for a GIF that was only ever SENT.
    QCOMPARE(recent.data(recent.index(0, 0),
                         GifResultModel::FavoriteRole).toBool(),
             true);
    // The collection tells the truth, which is why GifPicker.qml's isSaved()
    // asks it instead.
    QVERIFY(!favorites.isFavorite(QStringLiteral("giphy"),
                                  QStringLiteral("sent1")));

    // And it answers honestly once the GIF really is saved.
    QVERIFY(favorites.toggle(toMap(make("giphy", "sent1"))));
    QVERIFY(favorites.isFavorite(QStringLiteral("giphy"),
                                 QStringLiteral("sent1")));
    favorites.unfavorite(QStringLiteral("giphy"), QStringLiteral("sent1"));
    QVERIFY(!favorites.isFavorite(QStringLiteral("giphy"),
                                  QStringLiteral("sent1")));
}

QTEST_MAIN(GifCollectionsTest)
#include "GifCollectionsTest.moc"
