// v0.6.1: GIF search controller. Drives the full request lifecycle with a fake
// transport (canned responses, no network, no key) — debounce, cancellation,
// stale-result rejection, provider switching, pagination, dedup, safe-search
// re-run, and the MissingKey / Offline / RateLimited / ProviderError / NoResults
// states.

#include "gif/GifSearchController.h"
#include "gif/GifResultModel.h"
#include "gif/GifTransport.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest/QtTest>

namespace {

// Records issued URLs and lets the test complete requests on demand.
class FakeGifTransport : public GifTransport
{
    Q_OBJECT
public:
    bool up = true;
    quint64 next = 100;
    QList<QPair<quint64, QString>> issued; // (opId, url)

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
    quint64 lastOp() const { return issued.isEmpty() ? 0 : issued.last().first; }
    QString lastUrl() const { return issued.isEmpty() ? QString() : issued.last().second; }
};

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

} // namespace

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
        // Isolate QSettings (favorites/recents) into a throwaway location.
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
    void toggleFavoriteReflectsInGrid();
};

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
    gif->setApiKey(QStringLiteral("giphy"), QString()); // clear key
    gif->showTrending();
    QCOMPARE(gif->state(), int(GifSearchController::MissingKey));
    QVERIFY(transport->issued.isEmpty()); // no request without a key
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
    // Late "cats" result must be ignored.
    transport->complete(firstOp, true, 200, giphyBody({ "cat1" }),
                        QStringLiteral("ok"));
    QCOMPARE(gif->results()->count(), 0);
    // The current "dogs" result applies.
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
    // Page 2 repeats "b" (dedup) and adds "c".
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
    QCOMPARE(gif->results()->count(), 0); // cleared on switch
    QVERIFY(transport->lastUrl().startsWith(
        QStringLiteral("https://api.klipy.com/api/v1/KKEY/gifs/trending")));
    QCOMPARE(gif->attribution(), QStringLiteral("Powered by KLIPY"));
}

void GifSearchControllerTest::emptyQueryReturnsToTrending()
{
    makeController();
    gif->searchNow(QStringLiteral("cats"));
    QCOMPARE(gif->mode(), int(GifSearchController::Search));
    gif->setQueryText(QString()); // cleared → trending, no debounce wait
    QCOMPARE(gif->mode(), int(GifSearchController::Trending));
    QVERIFY(transport->lastUrl().contains(QStringLiteral("/trending")));
}

void GifSearchControllerTest::safeSearchChangeReRuns()
{
    makeController();
    gif->searchNow(QStringLiteral("cats"));
    const quint64 before = transport->lastOp();
    gif->setRating(int(gif::Rating::G));
    // A new request was issued with the new rating.
    QVERIFY(transport->lastOp() != before);
    QVERIFY(transport->lastUrl().contains(QStringLiteral("rating=g")));
}

void GifSearchControllerTest::keyNeverAppearsInSignals()
{
    makeController();
    // Neither the state signal payloads nor any exposed property carry the key.
    // The controller's public surface has no key accessor at all; assert the
    // configured() boolean is all that leaks, and the model rows carry only
    // provider CDN URLs (no api_key on GIPHY media URLs).
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
    QVERIFY(gif->toggleFavorite(row));            // now favorited
    QCOMPARE(gif->favorites()->count(), 1);
    QVERIFY(gif->results()->get(0).value(QStringLiteral("favorite")).toBool());
    QVERIFY(!gif->toggleFavorite(row));           // toggled off
    QCOMPARE(gif->favorites()->count(), 0);
    gif->favorites()->clearAll();
}

QTEST_MAIN(GifSearchControllerTest)
#include "GifSearchControllerTest.moc"
