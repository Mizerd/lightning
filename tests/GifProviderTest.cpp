// v0.6.1: shared GIF provider abstraction. Verifies endpoint construction,
// API-key injection, pagination convention, safe-search mapping, attribution,
// parse dispatch and the factory — for both GIPHY and KLIPY — without any
// network. Keys used here are placeholders, never real.

#include "gif/GifProvider.h"

#include <QUrl>
#include <QUrlQuery>
#include <QtTest/QtTest>

using gif::GifProvider;
using gif::Rating;

namespace {
QUrlQuery queryOf(const QString &url) { return QUrlQuery(QUrl(url).query()); }
} // namespace

class GifProviderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void factoryKnowsBothProviders();
    void giphyTrendingUrl();
    void giphySearchUrlEncodesAndClampsQuery();
    void giphyOffsetPagination();
    void klipyTrendingUrlEmbedsKeyInPath();
    void klipyPagePagination();
    void emptyKeyYieldsNoUrl();
    void emptySearchYieldsNoUrl();
    void attributionsDiffer();
    void parseDispatchMatchesProvider();
};

void GifProviderTest::factoryKnowsBothProviders()
{
    QVERIFY(gif::makeGifProvider(QStringLiteral("giphy")) != nullptr);
    QVERIFY(gif::makeGifProvider(QStringLiteral("klipy")) != nullptr);
    QVERIFY(gif::makeGifProvider(QStringLiteral("nope")) == nullptr);
    QCOMPARE(gif::knownGifProviderIds().size(), 2);
}

void GifProviderTest::giphyTrendingUrl()
{
    auto p = gif::makeGifProvider(QStringLiteral("giphy"));
    const QString url = p->trendingUrl(Rating::PG13, 0, 24, QStringLiteral("KEY"));
    QVERIFY(url.startsWith(
        QStringLiteral("https://api.giphy.com/v1/gifs/trending")));
    const QUrlQuery q = queryOf(url);
    QCOMPARE(q.queryItemValue(QStringLiteral("api_key")), QStringLiteral("KEY"));
    QCOMPARE(q.queryItemValue(QStringLiteral("limit")), QStringLiteral("24"));
    QCOMPARE(q.queryItemValue(QStringLiteral("offset")), QStringLiteral("0"));
    QCOMPARE(q.queryItemValue(QStringLiteral("rating")), QStringLiteral("pg-13"));
}

void GifProviderTest::giphySearchUrlEncodesAndClampsQuery()
{
    auto p = gif::makeGifProvider(QStringLiteral("giphy"));
    const QString longQuery(80, QLatin1Char('a'));
    const QString url =
        p->searchUrl(QStringLiteral("happy cat"), Rating::G, 0, 24,
                     QStringLiteral("KEY"));
    QVERIFY(url.startsWith(QStringLiteral("https://api.giphy.com/v1/gifs/search")));
    const QUrlQuery q = queryOf(url);
    QCOMPARE(q.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded),
             QStringLiteral("happy cat"));
    // 50-char cap.
    const QString clamped =
        queryOf(p->searchUrl(longQuery, Rating::G, 0, 24, QStringLiteral("K")))
            .queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
    QCOMPARE(clamped.size(), 50);
}

void GifProviderTest::giphyOffsetPagination()
{
    auto p = gif::makeGifProvider(QStringLiteral("giphy"));
    const QUrlQuery q =
        queryOf(p->trendingUrl(Rating::G, 3, 24, QStringLiteral("K")));
    QCOMPARE(q.queryItemValue(QStringLiteral("offset")), QStringLiteral("72"));
}

void GifProviderTest::klipyTrendingUrlEmbedsKeyInPath()
{
    auto p = gif::makeGifProvider(QStringLiteral("klipy"));
    const QString url = p->trendingUrl(Rating::G, 0, 24, QStringLiteral("SECRET"));
    QVERIFY(url.startsWith(
        QStringLiteral("https://api.klipy.com/api/v1/SECRET/gifs/trending")));
    const QUrlQuery q = queryOf(url);
    QCOMPARE(q.queryItemValue(QStringLiteral("per_page")), QStringLiteral("24"));
    QCOMPARE(q.queryItemValue(QStringLiteral("page")), QStringLiteral("1"));
    QCOMPARE(q.queryItemValue(QStringLiteral("rating")), QStringLiteral("g"));
}

void GifProviderTest::klipyPagePagination()
{
    auto p = gif::makeGifProvider(QStringLiteral("klipy"));
    // 0-based page 2 → KLIPY page 3; per_page clamped to [8,50].
    QUrlQuery q = queryOf(p->searchUrl(QStringLiteral("dog"), Rating::G, 2, 100,
                                       QStringLiteral("K")));
    QCOMPARE(q.queryItemValue(QStringLiteral("page")), QStringLiteral("3"));
    QCOMPARE(q.queryItemValue(QStringLiteral("per_page")), QStringLiteral("50"));
    QCOMPARE(q.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded),
             QStringLiteral("dog"));
    q = queryOf(p->trendingUrl(Rating::G, 0, 2, QStringLiteral("K")));
    QCOMPARE(q.queryItemValue(QStringLiteral("per_page")), QStringLiteral("8"));
}

void GifProviderTest::emptyKeyYieldsNoUrl()
{
    for (const QString &id : gif::knownGifProviderIds()) {
        auto p = gif::makeGifProvider(id);
        QVERIFY(p->trendingUrl(Rating::G, 0, 24, QString()).isEmpty());
        QVERIFY(p->searchUrl(QStringLiteral("x"), Rating::G, 0, 24, QString())
                    .isEmpty());
        QVERIFY(p->categoriesUrl(QString()).isEmpty());
    }
}

void GifProviderTest::emptySearchYieldsNoUrl()
{
    for (const QString &id : gif::knownGifProviderIds()) {
        auto p = gif::makeGifProvider(id);
        QVERIFY(p->searchUrl(QStringLiteral("   "), Rating::G, 0, 24,
                             QStringLiteral("K")).isEmpty());
    }
}

void GifProviderTest::attributionsDiffer()
{
    QCOMPARE(gif::makeGifProvider(QStringLiteral("giphy"))->attribution(),
             QStringLiteral("Powered by GIPHY"));
    QCOMPARE(gif::makeGifProvider(QStringLiteral("klipy"))->attribution(),
             QStringLiteral("Powered by KLIPY"));
}

void GifProviderTest::parseDispatchMatchesProvider()
{
    auto giphy = gif::makeGifProvider(QStringLiteral("giphy"));
    const QByteArray g =
        "{\"data\":[{\"id\":\"a\",\"rating\":\"g\",\"images\":{\"original\":{"
        "\"url\":\"https://media.giphy.com/media/a/giphy.gif\",\"width\":\"10\","
        "\"height\":\"10\",\"size\":\"1\"}}}],\"pagination\":{\"total_count\":1,"
        "\"count\":1,\"offset\":0}}";
    const auto go = giphy->parse(g, Rating::R, 0, 24);
    QVERIFY(go.ok);
    QCOMPARE(go.results.size(), 1);
    QCOMPARE(go.results.first().provider, QStringLiteral("giphy"));

    auto klipy = gif::makeGifProvider(QStringLiteral("klipy"));
    const QByteArray k =
        "{\"result\":true,\"data\":{\"data\":[{\"id\":7,\"type\":\"gif\","
        "\"title\":\"t\",\"file\":{\"md\":{\"gif\":{"
        "\"url\":\"https://static.klipy.com/x/7.gif\",\"width\":10,\"height\":10,"
        "\"size\":1}}}}],\"has_next\":false}}";
    const auto ko = klipy->parse(k, Rating::R, 0, 24);
    QVERIFY(ko.ok);
    QCOMPARE(ko.results.size(), 1);
    QCOMPARE(ko.results.first().provider, QStringLiteral("klipy"));
}

QTEST_GUILESS_MAIN(GifProviderTest)
#include "GifProviderTest.moc"
