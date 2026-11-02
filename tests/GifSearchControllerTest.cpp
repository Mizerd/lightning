
#include "gif/GifSearchController.h"
#include "gif/GifResultModel.h"
#include "gif/GifTransport.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QUrl>
#include <QtTest/QtTest>

namespace {

class FakeGifTransport : public GifTransport
{
    Q_OBJECT
public:
    bool up = true;
    quint64 next = 100;
    QList<QPair<quint64, QString>> issued;

    bool available() const override { return up; }
    quint64 get(const QString &url) override
    {
        if (!up)
            return 0;
        const quint64 op = next++;
        issued.append({ op, url });
        return op;
    }
    void complete(quint64 op, bool ok, int status, const QByteArray &body,
                  const QString &category)
    {
        Q_EMIT finished(op, ok, status, body, category);
    }
    QList<QPair<quint64, QString>> downloads;
    quint64 download(const QString &url) override
    {
        if (!up)
            return 0;
        const quint64 op = next++;
        downloads.append({ op, url });
        return op;
    }
    void completeDownload(quint64 op, bool ok, const QByteArray &bytes,
                          const QString &category = QString())
    {
        Q_EMIT downloadFinished(op, ok, bytes, category);
    }
    quint64 downloadOpFor(const QString &url) const
    {
        for (const auto &d : downloads) {
            if (d.second == url)
                return d.first;
        }
        return 0;
    }
    quint64 lastOp() const { return issued.isEmpty() ? 0 : issued.last().first; }
    QString lastUrl() const { return issued.isEmpty() ? QString() : issued.last().second; }
};

QByteArray tinyGif()
{
    QByteArray b("GIF89a");
    b.append(char(2)).append(char(0)).append(char(2)).append(char(0));
    b.append(QByteArray(32, '\0'));
    return b;
}

QString previewUrl(const QString &id)
{
    return QStringLiteral("https://media.giphy.com/media/%1/200w.gif").arg(id);
}

QByteArray giphyBody(const QStringList &ids, int total = 100)
{
    QByteArray items;
    for (int i = 0; i < ids.size(); ++i) {
        if (i)
            items += ",";
        items += "{\"id\":\"" + ids[i].toUtf8() + "\",\"rating\":\"g\","
                 "\"title\":\"t\",\"images\":{\"original\":{"
                 "\"url\":\"https://media.giphy.com/media/" + ids[i].toUtf8()
              + "/giphy.gif\",\"width\":\"100\",\"height\":\"100\",\"size\":\"10\"}}}";
    }
    return "{\"data\":[" + items + "],\"pagination\":{\"total_count\":"
        + QByteArray::number(total) + ",\"count\":" + QByteArray::number(ids.size())
        + ",\"offset\":0}}";
}

}

class GifSearchControllerTest : public QObject
{
    Q_OBJECT

    FakeGifTransport *transport = nullptr;
    GifSearchController *gif = nullptr;

    void makeController()
    {
        transport = new FakeGifTransport;
        gif = new GifSearchController;
        gif->setDebounceMs(1);
        gif->setApiKey(QStringLiteral("giphy"), QStringLiteral("GKEY"));
        gif->setApiKey(QStringLiteral("klipy"), QStringLiteral("KKEY"));
        gif->setTransport(transport);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("LightningTest"));
        QCoreApplication::setApplicationName(QStringLiteral("GifControllerTest"));
    }

    void cleanup()
    {
        delete gif; gif = nullptr;
        delete transport; transport = nullptr;
    }

    void trendingLoadsResults();
    void missingKeyStateWhenUnconfigured();
    void offlineWhenTransportUnavailable();
    void rateLimitedMapsState();
    void providerErrorOnMalformed();
    void noResultsOnEmpty();
    void staleResultIgnoredAfterNewQuery();
    void paginationAppendsAndDedupes();
    void providerSwitchResetsAndReRuns();
    void emptyQueryReturnsToTrending();
    void safeSearchChangeReRuns();
    void keyNeverAppearsInSignals();
    void defaultResolutionUsesEnvNotHardcoded();
    void toggleFavoriteReflectsInGrid();
    void previewRefusesSvgAndSvgz();
    void previewStoresValidatedGifAsPrivateFile();
    void previewFetchesOnlyProviderGifUrls();
    void previewFetchIsBoundedAndStillsGoFirst();
    void previewHoldsProtectFromEvictionAndDropQueued();
    void previewSessionEndClearsFiles();
    void previewLostAnswerFreesItsSlot();
    void previewClosedPickerFetchesNothing();
    void previewTransientFailuresRetryAndRefusalsDoNot();
};

void GifSearchControllerTest::defaultResolutionUsesEnvNotHardcoded()
{
    qunsetenv("LIGHTNING_GIPHY_API_KEY");
    qunsetenv("LIGHTNING_KLIPY_API_KEY");
    {
        GifSearchController c;
        QVERIFY(!c.providerConfigured(QStringLiteral("giphy")));
        QVERIFY(!c.providerConfigured(QStringLiteral("klipy")));
    }
    qputenv("LIGHTNING_GIPHY_API_KEY", QByteArray("ENV-SYNTHETIC-GIPHY"));
    {
        GifSearchController c;
        QVERIFY(c.providerConfigured(QStringLiteral("giphy")));
        QVERIFY(!c.providerConfigured(QStringLiteral("klipy")));
    }
    qunsetenv("LIGHTNING_GIPHY_API_KEY");
}

void GifSearchControllerTest::trendingLoadsResults()
{
    makeController();
    QSignalSpy stateSpy(gif, &GifSearchController::stateChanged);
    gif->showTrending();
    QCOMPARE(gif->state(), int(GifSearchController::Loading));
    QVERIFY(transport->lastUrl().startsWith(
        QStringLiteral("https://api.giphy.com/v1/gifs/trending")));
    transport->complete(transport->lastOp(), true, 200,
                        giphyBody({ "a", "b", "c" }), QStringLiteral("ok"));
    QCOMPARE(gif->state(), int(GifSearchController::Ready));
    QCOMPARE(gif->results()->count(), 3);
    QVERIFY(gif->hasMore());
}

void GifSearchControllerTest::missingKeyStateWhenUnconfigured()
{
    makeController();
    gif->setApiKey(QStringLiteral("giphy"), QString());
    gif->showTrending();
    QCOMPARE(gif->state(), int(GifSearchController::MissingKey));
    QVERIFY(transport->issued.isEmpty());
    QVERIFY(!gif->configured());
}

void GifSearchControllerTest::offlineWhenTransportUnavailable()
{
    makeController();
    transport->up = false;
    gif->showTrending();
    QCOMPARE(gif->state(), int(GifSearchController::Offline));
    QVERIFY(transport->issued.isEmpty());
}

void GifSearchControllerTest::rateLimitedMapsState()
{
    makeController();
    gif->showTrending();
    transport->complete(transport->lastOp(), false, 429, {},
                        QStringLiteral("rate_limited"));
    QCOMPARE(gif->state(), int(GifSearchController::RateLimited));
}

void GifSearchControllerTest::providerErrorOnMalformed()
{
    makeController();
    gif->showTrending();
    transport->complete(transport->lastOp(), true, 200, "not json",
                        QStringLiteral("ok"));
    QCOMPARE(gif->state(), int(GifSearchController::ProviderError));
}

void GifSearchControllerTest::noResultsOnEmpty()
{
    makeController();
    gif->searchNow(QStringLiteral("zzzznothing"));
    transport->complete(transport->lastOp(), true, 200,
                        "{\"data\":[],\"pagination\":{\"total_count\":0}}",
                        QStringLiteral("ok"));
    QCOMPARE(gif->state(), int(GifSearchController::NoResults));
    QCOMPARE(gif->results()->count(), 0);
}

void GifSearchControllerTest::staleResultIgnoredAfterNewQuery()
{
    makeController();
    gif->searchNow(QStringLiteral("cats"));
    const quint64 firstOp = transport->lastOp();
    gif->searchNow(QStringLiteral("dogs"));
    const quint64 secondOp = transport->lastOp();
    QVERIFY(firstOp != secondOp);
    transport->complete(firstOp, true, 200, giphyBody({ "cat1" }),
                        QStringLiteral("ok"));
    QCOMPARE(gif->results()->count(), 0);
    transport->complete(secondOp, true, 200, giphyBody({ "dog1", "dog2" }),
                        QStringLiteral("ok"));
    QCOMPARE(gif->results()->count(), 2);
}

void GifSearchControllerTest::paginationAppendsAndDedupes()
{
    makeController();
    gif->showTrending();
    transport->complete(transport->lastOp(), true, 200,
                        giphyBody({ "a", "b" }), QStringLiteral("ok"));
    QCOMPARE(gif->results()->count(), 2);
    gif->loadMore();
    QCOMPARE(gif->state(), int(GifSearchController::LoadingMore));
    QVERIFY(transport->lastUrl().contains(QStringLiteral("offset=24")));
    transport->complete(transport->lastOp(), true, 200,
                        giphyBody({ "b", "c" }), QStringLiteral("ok"));
    QCOMPARE(gif->results()->count(), 3);
    QCOMPARE(gif->state(), int(GifSearchController::Ready));
}

void GifSearchControllerTest::providerSwitchResetsAndReRuns()
{
    makeController();
    gif->showTrending();
    transport->complete(transport->lastOp(), true, 200,
                        giphyBody({ "a", "b" }), QStringLiteral("ok"));
    QCOMPARE(gif->results()->count(), 2);
    gif->setActiveProvider(QStringLiteral("klipy"));
    QCOMPARE(gif->providerId(), QStringLiteral("klipy"));
    QCOMPARE(gif->results()->count(), 0);
    QVERIFY(transport->lastUrl().startsWith(
        QStringLiteral("https://api.klipy.com/api/v1/KKEY/gifs/trending")));
    QCOMPARE(gif->attribution(), QStringLiteral("Powered by KLIPY"));
}

void GifSearchControllerTest::emptyQueryReturnsToTrending()
{
    makeController();
    gif->searchNow(QStringLiteral("cats"));
    QCOMPARE(gif->mode(), int(GifSearchController::Search));
    gif->setQueryText(QString());
    QCOMPARE(gif->mode(), int(GifSearchController::Trending));
    QVERIFY(transport->lastUrl().contains(QStringLiteral("/trending")));
}

void GifSearchControllerTest::safeSearchChangeReRuns()
{
    makeController();
    gif->searchNow(QStringLiteral("cats"));
    const quint64 before = transport->lastOp();
    gif->setRating(int(gif::Rating::G));
    QVERIFY(transport->lastOp() != before);
    QVERIFY(transport->lastUrl().contains(QStringLiteral("rating=g")));
}

void GifSearchControllerTest::keyNeverAppearsInSignals()
{
    makeController();
    gif->showTrending();
    transport->complete(transport->lastOp(), true, 200,
                        giphyBody({ "a" }), QStringLiteral("ok"));
    const QVariantMap row = gif->results()->get(0);
    QVERIFY(!row.value(QStringLiteral("gifUrl")).toString()
                 .contains(QStringLiteral("GKEY")));
    QVERIFY(!row.value(QStringLiteral("gifUrl")).toString()
                 .contains(QStringLiteral("api_key")));
}

void GifSearchControllerTest::toggleFavoriteReflectsInGrid()
{
    makeController();
    gif->favorites()->clearAll();
    gif->showTrending();
    transport->complete(transport->lastOp(), true, 200,
                        giphyBody({ "fav1" }), QStringLiteral("ok"));
    const QVariantMap row = gif->results()->get(0);
    QVERIFY(!row.value(QStringLiteral("favorite")).toBool());
    QVERIFY(gif->toggleFavorite(row));
    QCOMPARE(gif->favorites()->count(), 1);
    QVERIFY(gif->results()->get(0).value(QStringLiteral("favorite")).toBool());
    QVERIFY(!gif->toggleFavorite(row));
    QCOMPARE(gif->favorites()->count(), 0);
    gif->favorites()->clearAll();
}

void GifSearchControllerTest::previewRefusesSvgAndSvgz()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    const QList<QByteArray> hostile = {
        QByteArrayLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"),
        QByteArrayLiteral("\xEF\xBB\xBF \n<?xml version=\"1.0\"?><svg/>"),
        QByteArrayLiteral("\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\x03"),
        QByteArrayLiteral("  <svg><!-- GIF89a --></svg>"),
    };
    for (int i = 0; i < hostile.size(); ++i) {
        const QString url = previewUrl(QStringLiteral("svg%1").arg(i));
        QVERIFY(previews->source(url).isEmpty());
        const quint64 op = transport->downloadOpFor(url);
        QVERIFY(op != 0);
        QSignalSpy revision(previews, &GifPreviewCache::revisionChanged);
        transport->completeDownload(op, true, hostile.at(i));
        QVERIFY2(previews->source(url).isEmpty(), qPrintable(url));
        QCOMPARE(revision.count(), 0);
        const int issued = transport->downloads.size();
        QVERIFY(previews->source(url).isEmpty());
        QCOMPARE(transport->downloads.size(), issued);
    }
    QCOMPARE(previews->cachedCountForTest(), 0);
}

void GifSearchControllerTest::previewStoresValidatedGifAsPrivateFile()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    const QString url = previewUrl(QStringLiteral("ok"));
    QVERIFY(previews->source(url).isEmpty());
    QSignalSpy revision(previews, &GifPreviewCache::revisionChanged);
    transport->completeDownload(transport->downloadOpFor(url), true, tinyGif());
    QCOMPARE(revision.count(), 1);
    const QString source = previews->source(url);
    QVERIFY(source.startsWith(QStringLiteral("file://")));
    QFile file(QUrl(source).toLocalFile());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), tinyGif());
#ifdef Q_OS_UNIX
    QVERIFY(!(file.permissions()
              & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                 | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
#endif
    const int issued = transport->downloads.size();
    QCOMPARE(previews->source(url), source);
    QCOMPARE(transport->downloads.size(), issued);
}

void GifSearchControllerTest::previewFetchesOnlyProviderGifUrls()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    for (const QString &url : {
             QStringLiteral("https://evil.example/media/a.gif"),
             QStringLiteral("http://media.giphy.com/media/a/200w.gif"),
             QStringLiteral("https://media.giphy.com/media/a/200w.svg"),
             QStringLiteral("https://media.giphy.com.evil.example/a.gif"),
             QStringLiteral("file:///tmp/a.gif"),
         }) {
        QVERIFY2(previews->source(url).isEmpty(), qPrintable(url));
    }
    QVERIFY(transport->downloads.isEmpty());
    QVERIFY(previews->source(
        QStringLiteral("https://static.klipy.com/a/sm.gif")).isEmpty());
    QCOMPARE(transport->downloads.size(), 1);
}

void GifSearchControllerTest::previewFetchIsBoundedAndStillsGoFirst()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    const int extra = 3;
    for (int i = 0; i < GifPreviewCache::kMaxConcurrent + extra; ++i)
        previews->source(previewUrl(QStringLiteral("p%1").arg(i)));
    QCOMPARE(transport->downloads.size(), GifPreviewCache::kMaxConcurrent);
    QCOMPARE(previews->queuedCountForTest(), extra);
    previews->source(previewUrl(QStringLiteral("p0")));
    previews->source(previewUrl(QStringLiteral("p%1")
                                    .arg(GifPreviewCache::kMaxConcurrent)));
    QCOMPARE(previews->queuedCountForTest(), extra);

    const QString still = previewUrl(QStringLiteral("still"));
    previews->source(still, true);
    transport->completeDownload(transport->downloads.first().first, true,
                                tinyGif());
    QCOMPARE(transport->downloads.last().second, still);
    QCOMPARE(previews->inflightCountForTest(), GifPreviewCache::kMaxConcurrent);

    gif->reset();
    QVERIFY(!previews->isActive());
    QCOMPARE(previews->queuedCountForTest(), 0);
    QSignalSpy revision(previews, &GifPreviewCache::revisionChanged);
    const QString late = previewUrl(QStringLiteral("p1"));
    transport->completeDownload(transport->downloadOpFor(late), true, tinyGif());
    QCOMPARE(revision.count(), 1);
    QVERIFY(!previews->source(late).isEmpty());
    const int issued = transport->downloads.size();
    for (int i = 0; i < GifPreviewCache::kMaxConcurrent + extra; ++i)
        previews->source(previewUrl(QStringLiteral("closed%1").arg(i)));
    previews->source(previewUrl(QStringLiteral("closedStill")), true);
    QCOMPARE(previews->queuedCountForTest(), 0);
    QCOMPARE(transport->downloads.size(), issued);

    gif->notifyPickerOpening(QStringLiteral("room"));
    QVERIFY(previews->isActive());
    QCOMPARE(revision.count(), 2);
    for (const auto &d : std::as_const(transport->downloads))
        transport->completeDownload(d.first, false, {}, QStringLiteral("network"));
    QCOMPARE(previews->inflightCountForTest(), 0);
    previews->source(previewUrl(QStringLiteral("reopened")));
    QCOMPARE(transport->downloads.last().second,
             previewUrl(QStringLiteral("reopened")));
}

void GifSearchControllerTest::previewLostAnswerFreesItsSlot()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    for (int i = 0; i < GifPreviewCache::kMaxConcurrent + 1; ++i)
        previews->source(previewUrl(QStringLiteral("lost%1").arg(i)));
    QCOMPARE(previews->inflightCountForTest(), GifPreviewCache::kMaxConcurrent);
    QCOMPARE(previews->queuedCountForTest(), 1);
    const quint64 firstOp = transport->downloads.first().first;

    previews->expireStaleFetches();
    QCOMPARE(previews->inflightCountForTest(), GifPreviewCache::kMaxConcurrent);
    previews->setFetchTimeoutMsForTest(0);
    previews->expireStaleFetches();
    QCOMPARE(previews->queuedCountForTest(), 0);
    QCOMPARE(previews->inflightCountForTest(), 1);
    QCOMPARE(transport->downloads.last().second,
             previewUrl(QStringLiteral("lost%1").arg(GifPreviewCache::kMaxConcurrent)));
    transport->completeDownload(firstOp, true, tinyGif());
    QCOMPARE(previews->cachedCountForTest(), 0);
}

void GifSearchControllerTest::previewHoldsProtectFromEvictionAndDropQueued()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    previews->setLimitsForTest(1, 1024 * 1024);
    const QString a = previewUrl(QStringLiteral("a"));
    const QString b = previewUrl(QStringLiteral("b"));
    auto *owner = new QObject;
    previews->hold(owner, { a });
    previews->source(a);
    previews->source(b);
    transport->completeDownload(transport->downloadOpFor(a), true, tinyGif());
    transport->completeDownload(transport->downloadOpFor(b), true, tinyGif());
    QVERIFY(!previews->source(a).isEmpty());
    QCOMPARE(previews->cachedCountForTest(), 1);

    for (int i = 0; i < GifPreviewCache::kMaxConcurrent; ++i)
        previews->source(previewUrl(QStringLiteral("busy%1").arg(i)));
    const QString queued = previewUrl(QStringLiteral("queued"));
    previews->hold(owner, { a, queued });
    previews->source(queued);
    QCOMPARE(previews->queuedCountForTest(), 1);
    delete owner;
    QCOMPARE(previews->queuedCountForTest(), 0);
    previews->source(previewUrl(QStringLiteral("c")));
    transport->completeDownload(
        transport->downloadOpFor(previewUrl(QStringLiteral("busy0"))), true,
        tinyGif());
    QCOMPARE(previews->cachedCountForTest(), 1);
    QVERIFY(previews->source(a).isEmpty());
}

void GifSearchControllerTest::previewSessionEndClearsFiles()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    const QString url = previewUrl(QStringLiteral("gone"));
    previews->source(url);
    transport->completeDownload(transport->downloadOpFor(url), true, tinyGif());
    const QString path = QUrl(previews->source(url)).toLocalFile();
    QVERIFY(QFile::exists(path));
    Q_EMIT transport->sessionEnded();
    QVERIFY(!QFile::exists(path));
    QCOMPARE(previews->cachedCountForTest(), 0);
}

void GifSearchControllerTest::previewClosedPickerFetchesNothing()
{
    makeController();
    GifPreviewCache *previews = gif->previews();
    QVERIFY(!previews->isActive());
    QVERIFY(previews->source(previewUrl(QStringLiteral("x"))).isEmpty());
    QVERIFY(transport->downloads.isEmpty());
    QCOMPARE(previews->queuedCountForTest(), 0);
}

void GifSearchControllerTest::previewTransientFailuresRetryAndRefusalsDoNot()
{
    makeController();
    gif->notifyPickerOpening(QStringLiteral("room"));
    GifPreviewCache *previews = gif->previews();
    previews->setRetryAfterMsForTest(0);
    const QStringList transient = { QStringLiteral("gone"),
                                    QStringLiteral("network"),
                                    QStringLiteral("timeout") };
    const QStringList permanent = { QStringLiteral("not_a_gif"),
                                    QStringLiteral("too_large"),
                                    QStringLiteral("blocked") };
    for (const QString &category : transient + permanent) {
        const QString url = previewUrl(category);
        previews->source(url);
        transport->completeDownload(transport->downloadOpFor(url), false, {},
                                    category);
        const int issued = transport->downloads.size();
        previews->source(url);
        QCOMPARE(transport->downloads.size(),
                 issued + (transient.contains(category) ? 1 : 0));
    }
}

QTEST_MAIN(GifSearchControllerTest)
#include "GifSearchControllerTest.moc"
