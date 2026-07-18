// v0.6.1: client-side GIF provider response parsing. Exercises gif::parseGiphy
// and its safety helpers without any network, API key, or homeserver: variant
// selection, provider-CDN host + .gif validation, tracking-param stripping,
// safe-search rating filtering (including unknown ratings), size/dimension
// caps, dedup, pagination offset, and malformed-input rejection.

#include "gif/GifResponseParser.h"

#include <QtTest/QtTest>

using gif::GifResult;
using gif::ParseOutcome;
using gif::Rating;

namespace {

// One GIPHY-shaped result object (numbers as strings, as GIPHY sends them).
QByteArray giphyResponse(const QByteArray &dataItems, int totalCount = 42)
{
    return "{\"data\":[" + dataItems + "],"
           "\"pagination\":{\"total_count\":" + QByteArray::number(totalCount)
           + ",\"count\":1,\"offset\":0},\"meta\":{\"status\":200}}";
}

QByteArray gifItem(const QByteArray &id, const QByteArray &rating)
{
    return "{\"id\":\"" + id + "\",\"title\":\"a cat\",\"rating\":\"" + rating
        + "\",\"images\":{"
          "\"original\":{\"url\":\"https://media.giphy.com/media/" + id
        + "/giphy.gif?cid=abc&rid=def&ct=g\",\"width\":\"480\",\"height\":\"270\","
          "\"size\":\"1048576\",\"mp4\":\"https://media.giphy.com/media/" + id
        + "/giphy.mp4?cid=abc\"},"
          "\"fixed_width\":{\"url\":\"https://media.giphy.com/media/" + id
        + "/200w.gif?cid=abc\",\"width\":\"200\",\"height\":\"112\"},"
          "\"fixed_width_still\":{\"url\":\"https://media.giphy.com/media/" + id
        + "/200w_s.jpg\",\"width\":\"200\",\"height\":\"112\"}}}";
}

} // namespace

class GifResponseParserTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesTrendingResult();
    void stripsTrackingParams();
    void rejectsNonGiphyHost();
    void rejectsNonGifPath();
    void safeSearchFiltersHigherRatings();
    void safeSearchExcludesUnknownRating();
    void dedupesById();
    void enforcesDimensionCap();
    void enforcesByteCap();
    void skipsResultsWithoutSendableGif();
    void malformedJsonIsNotOk();
    void emptyDataIsOkButEmpty();
    void paginationOffsetAdvances();
    void ratingHelpers();
};

void GifResponseParserTest::parsesTrendingResult()
{
    const auto out = gif::parseGiphy(giphyResponse(gifItem("abc", "g")),
                                     Rating::PG13, 0);
    QVERIFY(out.ok);
    QCOMPARE(out.results.size(), 1);
    const GifResult &r = out.results.first();
    QCOMPARE(r.id, QStringLiteral("abc"));
    QCOMPARE(r.title, QStringLiteral("a cat"));
    QCOMPARE(r.rating, QStringLiteral("g"));
    // Sendable variant = original .gif, tracking stripped.
    QCOMPARE(r.gifUrl,
             QStringLiteral("https://media.giphy.com/media/abc/giphy.gif"));
    QCOMPARE(r.gifWidth, 480);
    QCOMPARE(r.gifHeight, 270);
    QCOMPARE(r.gifBytes, Q_INT64_C(1048576));
    // Preview = smaller animated rendition.
    QCOMPARE(r.previewUrl,
             QStringLiteral("https://media.giphy.com/media/abc/200w.gif"));
    QCOMPARE(r.previewWidth, 200);
    // Still thumbnail present, mp4 preview captured but never a gif.
    QCOMPARE(r.stillUrl,
             QStringLiteral("https://media.giphy.com/media/abc/200w_s.jpg"));
    QCOMPARE(r.mp4Url,
             QStringLiteral("https://media.giphy.com/media/abc/giphy.mp4"));
    QCOMPARE(out.totalCount, 42);
}

void GifResponseParserTest::stripsTrackingParams()
{
    QCOMPARE(gif::stripTracking(
                 QStringLiteral("https://media.giphy.com/media/x/giphy.gif?cid=1&ct=g")),
             QStringLiteral("https://media.giphy.com/media/x/giphy.gif"));
}

void GifResponseParserTest::rejectsNonGiphyHost()
{
    QVERIFY(!gif::isSendableGifUrl(
        QStringLiteral("https://evil.example.com/x/giphy.gif")));
    QVERIFY(gif::isSendableGifUrl(
        QStringLiteral("https://media0.giphy.com/media/x/giphy.gif")));
    QVERIFY(gif::isSendableGifUrl(
        QStringLiteral("https://giphy.com/media/x/giphy.gif")));
    // A look-alike host must not slip through.
    QVERIFY(!gif::isSendableGifUrl(
        QStringLiteral("https://giphy.com.evil.net/x/giphy.gif")));
}

void GifResponseParserTest::rejectsNonGifPath()
{
    // http (not https), mp4, and webp are never sendable as a gif.
    QVERIFY(!gif::isSendableGifUrl(
        QStringLiteral("http://media.giphy.com/media/x/giphy.gif")));
    QVERIFY(!gif::isSendableGifUrl(
        QStringLiteral("https://media.giphy.com/media/x/giphy.mp4")));
    QVERIFY(!gif::isSendableGifUrl(
        QStringLiteral("https://media.giphy.com/media/x/giphy.webp")));
}

void GifResponseParserTest::safeSearchFiltersHigherRatings()
{
    const QByteArray body = giphyResponse(
        gifItem("g1", "g") + "," + gifItem("r1", "r") + ","
        + gifItem("p1", "pg-13"));
    const auto pg = gif::parseGiphy(body, Rating::PG, 0);
    QVERIFY(pg.ok);
    QCOMPARE(pg.results.size(), 1);   // only the "g" result survives a PG cap
    QCOMPARE(pg.results.first().id, QStringLiteral("g1"));

    const auto r = gif::parseGiphy(body, Rating::R, 0);
    QCOMPARE(r.results.size(), 3);    // R allows everything
}

void GifResponseParserTest::safeSearchExcludesUnknownRating()
{
    const auto out = gif::parseGiphy(giphyResponse(gifItem("u1", "")),
                                     Rating::PG13, 0);
    QVERIFY(out.ok);
    QVERIFY(out.results.isEmpty());   // missing rating treated as R → excluded
    // …but an R cap lets an unrated item through.
    const auto allowed = gif::parseGiphy(giphyResponse(gifItem("u1", "")),
                                         Rating::R, 0);
    QCOMPARE(allowed.results.size(), 1);
}

void GifResponseParserTest::dedupesById()
{
    const auto out = gif::parseGiphy(
        giphyResponse(gifItem("dup", "g") + "," + gifItem("dup", "g")),
        Rating::R, 0);
    QCOMPARE(out.results.size(), 1);
}

void GifResponseParserTest::enforcesDimensionCap()
{
    QByteArray huge =
        "{\"id\":\"big\",\"rating\":\"g\",\"images\":{\"original\":{"
        "\"url\":\"https://media.giphy.com/media/big/giphy.gif\","
        "\"width\":\"9000\",\"height\":\"9000\",\"size\":\"1000\"}}}";
    const auto out = gif::parseGiphy(giphyResponse(huge), Rating::R, 0);
    QVERIFY(out.ok);
    QVERIFY(out.results.isEmpty());   // over kMaxGifDimension
}

void GifResponseParserTest::enforcesByteCap()
{
    QByteArray fat =
        "{\"id\":\"fat\",\"rating\":\"g\",\"images\":{\"original\":{"
        "\"url\":\"https://media.giphy.com/media/fat/giphy.gif\","
        "\"width\":\"400\",\"height\":\"400\",\"size\":\"999999999\"}}}";
    const auto out = gif::parseGiphy(giphyResponse(fat), Rating::R, 0);
    QVERIFY(out.results.isEmpty());   // over kMaxGifBytes
}

void GifResponseParserTest::skipsResultsWithoutSendableGif()
{
    // Only an mp4 rendition — nothing to send as m.image.
    QByteArray mp4Only =
        "{\"id\":\"vid\",\"rating\":\"g\",\"images\":{\"original\":{"
        "\"mp4\":\"https://media.giphy.com/media/vid/giphy.mp4\"}}}";
    const auto out = gif::parseGiphy(giphyResponse(mp4Only), Rating::R, 0);
    QVERIFY(out.ok);
    QVERIFY(out.results.isEmpty());
}

void GifResponseParserTest::malformedJsonIsNotOk()
{
    QVERIFY(!gif::parseGiphy("not json", Rating::R, 0).ok);
    QVERIFY(!gif::parseGiphy("{\"data\":\"oops\"}", Rating::R, 0).ok);
    QCOMPARE(gif::parseGiphy("not json", Rating::R, 0).errorCategory,
             QStringLiteral("malformed"));
}

void GifResponseParserTest::emptyDataIsOkButEmpty()
{
    const auto out = gif::parseGiphy("{\"data\":[]}", Rating::R, 0);
    QVERIFY(out.ok);                  // no-results is not an error
    QVERIFY(out.results.isEmpty());
    QVERIFY(out.errorCategory.isEmpty());
}

void GifResponseParserTest::paginationOffsetAdvances()
{
    const auto out = gif::parseGiphy(
        giphyResponse(gifItem("a", "g") + "," + gifItem("b", "g")), Rating::R,
        50);
    QCOMPARE(out.nextOffset, 52);     // requestOffset + returned count
}

void GifResponseParserTest::ratingHelpers()
{
    QVERIFY(gif::ratingWithin(QStringLiteral("g"), Rating::PG));
    QVERIFY(!gif::ratingWithin(QStringLiteral("r"), Rating::PG13));
    QVERIFY(gif::ratingWithin(QStringLiteral("pg-13"), Rating::PG13));
    QCOMPARE(static_cast<int>(gif::ratingFromString(QStringLiteral("PG"))),
             static_cast<int>(Rating::PG));
    // Unknown → most permissive.
    QCOMPARE(static_cast<int>(gif::ratingFromString(QStringLiteral("weird"))),
             static_cast<int>(Rating::R));
}

QTEST_GUILESS_MAIN(GifResponseParserTest)
#include "GifResponseParserTest.moc"
