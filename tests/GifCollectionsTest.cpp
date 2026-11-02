
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

}

class GifCollectionsTest : public QObject
{
    Q_OBJECT

    QTemporaryDir tempDir;
    QSettings *store = nullptr;

    QString storePath(const QString &name) const
    { return tempDir.filePath(name + QStringLiteral(".ini")); }

private Q_SLOTS:
    void initTestCase() { QVERIFY(tempDir.isValid()); }

    void init()
    {
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

    void starredInsertDedupsByHashAndOrdersNewestFirst();
    void starredPersistsAcrossReloadWithNoUrlFields();
    void starredUnstarRemovesEntry();
    void starredTotalBytesSumsRows();
    void starredNoSensitiveFieldsPersisted();
    void reopenReplacesRowsAndCanGoStorageless();

    void savedListsLocalRowsFirstThenProviderRows();
    void savedGetIsBoundsCheckedAcrossTheGroupBoundary();
    void savedTracksBothSourcesLive();
    void savedForwardsRolesCorrectlyForBothGroups();
    void savedSurvivesASourceReopen();
    void favoriteRoleIsAConstantAndNotASavedStateOracle();

    void localExtRoundTripsThroughJsonWhenSet();
    void missingExtFieldDefaultsToEmptyNeverCrashes();
    void favoritesEntryNeverGainsAnExtKey();
};

void GifCollectionsTest::favoriteToggleAndDedup()
{
    GifFavoritesModel fav(store);
    QVERIFY(fav.toggle(toMap(make("giphy", "a"))));
    QCOMPARE(fav.count(), 1);
    QVERIFY(fav.isFavorite(QStringLiteral("giphy"), QStringLiteral("a")));
    QVERIFY(!fav.toggle(toMap(make("giphy", "a"))));
    QCOMPARE(fav.count(), 0);
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
    fav.toggle(toMap(make("klipy", "1")));
    QCOMPARE(fav.count(), 2);
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
             QStringLiteral("b"));
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
    QVERIFY(recent.count() <= 60);
    QCOMPARE(recent.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("79"));
}

void GifCollectionsTest::recentRecordingDisabled()
{
    GifRecentModel recent(store);
    recent.recordSent(make("giphy", "keep"));
    recent.setRecordingEnabled(false);
    recent.recordSent(make("giphy", "dropped"));
    QCOMPARE(recent.count(), 1);
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
    QCOMPARE(reloaded.count(), 0);
}

void GifCollectionsTest::corruptedStoreRecovers()
{
    store->setValue(QStringLiteral("gif/favorites"),
                    QStringLiteral("{not valid json"));
    store->sync();
    GifFavoritesModel fav(store);
    QCOMPARE(fav.count(), 0);
    QVERIFY(fav.toggle(toMap(make("giphy", "ok"))));
}

void GifCollectionsTest::malformedStoreShapesAreAllSurvivable()
{
    const QString key = QStringLiteral("gif/favorites");
    for (const QString &payload : {
             QStringLiteral("{\"provider\":\"giphy\"}"),
             QStringLiteral("\"a string\""),
             QStringLiteral("42"),
             QStringLiteral("null"),
             QStringLiteral("[1,2,3]"),
             QStringLiteral("[null,null]"),
             QStringLiteral("[[],[]]"),
             QStringLiteral("[\"x\"]"),
             QStringLiteral("[{}]"),
             QStringLiteral("[{\"provider\":\"giphy\"}]"),
             QStringLiteral("[{\"id\":\"only\"}]"),
             QStringLiteral("[{\"provider\":5,\"id\":true,\"gifUrl\":[]}]"),
             QStringLiteral("[{\"provider\":\"giphy\",\"id\":\"a\","),
             QStringLiteral("["),
             QStringLiteral(""),
         }) {
        store->setValue(key, payload);
        store->sync();
        GifFavoritesModel fav(store);
        QCOMPARE(fav.count(), 0);
        QVERIFY2(fav.toggle(toMap(make("giphy", "ok"))), qPrintable(payload));
        QCOMPARE(fav.count(), 1);
        store->remove(key);
    }
}

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
    QCOMPARE(starred.get(0).value(QStringLiteral("gifId")).toString(), h2);
    QVERIFY(starred.hasHash(h1));
    QVERIFY(starred.hasHash(h2));

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

    starred.reopen(nullptr);
    QCOMPARE(starred.count(), 0);

    QSettings other(storePath(QStringLiteral("account-b")),
                    QSettings::IniFormat);
    other.clear();
    starred.reopen(&other);
    QCOMPARE(starred.count(), 0);
    starred.insertLocal(makeLocal(QString(64, QLatin1Char('g'))));
    QCOMPARE(starred.count(), 1);
    other.sync();

    store->sync();
    GifStarredModel original(store);
    QCOMPARE(original.count(), 1);
    QCOMPARE(original.get(0).value(QStringLiteral("gifId")).toString(),
             QString(64, QLatin1Char('f')));
    other.clear();
}

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

    QCOMPARE(saved.count(), 4);
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(), h2);
    QCOMPARE(saved.get(1).value(QStringLiteral("gifId")).toString(), h1);
    QCOMPARE(saved.get(2).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p2"));
    QCOMPARE(saved.get(3).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
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

    local.reopen(nullptr);
    QVERIFY(reset.count() >= 1);
    QVERIFY(counted.count() >= 1);
    QCOMPARE(saved.count(), 1);
    QCOMPARE(saved.get(0).value(QStringLiteral("provider")).toString(),
             QStringLiteral("giphy"));
    QCOMPARE(saved.get(0).value(QStringLiteral("gifId")).toString(),
             QStringLiteral("p1"));
    QVERIFY(saved.get(1).isEmpty());

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

void GifCollectionsTest::favoriteRoleIsAConstantAndNotASavedStateOracle()
{
    GifRecentModel recent(store);
    GifFavoritesModel favorites(store);

    recent.recordSent(make("giphy", "sent1"));
    QCOMPARE(recent.count(), 1);
    QCOMPARE(recent.data(recent.index(0, 0),
                         GifResultModel::FavoriteRole).toBool(),
             true);
    QVERIFY(!favorites.isFavorite(QStringLiteral("giphy"),
                                  QStringLiteral("sent1")));

    QVERIFY(favorites.toggle(toMap(make("giphy", "sent1"))));
    QVERIFY(favorites.isFavorite(QStringLiteral("giphy"),
                                 QStringLiteral("sent1")));
    favorites.unfavorite(QStringLiteral("giphy"), QStringLiteral("sent1"));
    QVERIFY(!favorites.isFavorite(QStringLiteral("giphy"),
                                  QStringLiteral("sent1")));
}

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

void GifCollectionsTest::missingExtFieldDefaultsToEmptyNeverCrashes()
{
    const QString h = QString(64, QLatin1Char('2'));
    store->setValue(QStringLiteral("gif/starred"),
                    QStringLiteral("[{\"provider\":\"local\",\"id\":\"%1\","
                                   "\"w\":10,\"h\":10,\"bytes\":10}]")
                        .arg(h));
    store->sync();
    GifStarredModel starred(store);
    QCOMPARE(starred.count(), 1);
    QCOMPARE(starred.resultAt(0).localExt, QString());
}

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
