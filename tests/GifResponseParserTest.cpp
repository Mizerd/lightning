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
    void hasMoreReflectsTotalCount();
    // KLIPY
    void parsesKlipyResult();
    void klipyRejectsNonKlipyHost();
    void klipyResultFalseIsMalformed();
    void klipyPageAdvancesAndHasNext();
    void klipySkipsNonGifType();
    void resultsCarryProviderIdentity();
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

void GifResponseParserTest::hasMoreReflectsTotalCount()
{
    // total_count 42, offset 0, 1 returned → more pages exist.
    const auto more = gif::parseGiphy(giphyResponse(gifItem("a", "g"), 42),
                                      Rating::R, 0);
    QVERIFY(more.hasMore);
    // At the end (offset already past total) → no more.
    const auto end = gif::parseGiphy(giphyResponse(gifItem("z", "g"), 1),
                                     Rating::R, 5);
    QVERIFY(!end.hasMore);
}

// ── KLIPY ────────────────────────────────────────────────────────────────
namespace {
QByteArray klipyItem(const QByteArray &id)
{
    return "{\"id\":" + id + ",\"slug\":\"cat\",\"title\":\"a cat\","
           "\"type\":\"gif\",\"file\":{"
           "\"md\":{\"gif\":{\"url\":\"https://static.klipy.com/ii/x/md/"
        + id + ".gif\",\"width\":400,\"height\":400,\"size\":1846435},"
           "\"jpg\":{\"url\":\"https://static.klipy.com/ii/x/md/" + id
        + ".jpg\",\"width\":400,\"height\":400,\"size\":25000},"
           "\"mp4\":{\"url\":\"https://static.klipy.com/ii/x/md/" + id
        + ".mp4\",\"width\":400,\"height\":400,\"size\":246501}},"
           "\"sm\":{\"gif\":{\"url\":\"https://static.klipy.com/ii/x/sm/" + id
        + ".gif\",\"width\":200,\"height\":200,\"size\":90000}}}}";
}
QByteArray klipyResponse(const QByteArray &items, bool hasNext = true)
{
    return "{\"result\":true,\"data\":{\"data\":[" + items
        + "],\"current_page\":1,\"per_page\":24,\"has_next\":"
        + (hasNext ? "true" : "false") + "}}";
}
} // namespace

void GifResponseParserTest::parsesKlipyResult()
{
    const auto out = gif::parseKlipy(klipyResponse(klipyItem("111")),
                                     Rating::PG13, 0);
    QVERIFY(out.ok);
    QCOMPARE(out.results.size(), 1);
    const GifResult &r = out.results.first();
    QCOMPARE(r.provider, QStringLiteral("klipy"));
    QCOMPARE(r.id, QStringLiteral("111"));
    QCOMPARE(r.title, QStringLiteral("a cat"));
    // Sendable = md .gif on a klipy host.
    QCOMPARE(r.gifUrl,
             QStringLiteral("https://static.klipy.com/ii/x/md/111.gif"));
    QCOMPARE(r.gifWidth, 400);
    QCOMPARE(r.gifBytes, Q_INT64_C(1846435));
    // Preview = smaller sm .gif.
    QCOMPARE(r.previewUrl,
             QStringLiteral("https://static.klipy.com/ii/x/sm/111.gif"));
    // Still = jpg; mp4 captured but never a gif.
    QCOMPARE(r.stillUrl,
             QStringLiteral("https://static.klipy.com/ii/x/md/111.jpg"));
    QCOMPARE(r.mp4Url,
             QStringLiteral("https://static.klipy.com/ii/x/md/111.mp4"));
    // No per-item rating → request rating stamped.
    QCOMPARE(r.rating, QStringLiteral("pg-13"));
}

void GifResponseParserTest::klipyRejectsNonKlipyHost()
{
    QByteArray evil =
        "{\"id\":9,\"type\":\"gif\",\"file\":{\"md\":{\"gif\":{"
        "\"url\":\"https://static.evil.com/x/md/9.gif\",\"width\":10,"
        "\"height\":10,\"size\":1}}}}";
    const auto out = gif::parseKlipy(klipyResponse(evil), Rating::R, 0);
    QVERIFY(out.ok);
    QVERIFY(out.results.isEmpty()); // wrong CDN host dropped
    // GIPHY host must not be accepted by the KLIPY host policy either.
    QVERIFY(gif::isSendableGifUrlForHosts(
        QStringLiteral("https://static.klipy.com/x.gif"),
        { QStringLiteral(".klipy.com") }));
    QVERIFY(!gif::isSendableGifUrlForHosts(
        QStringLiteral("https://media.giphy.com/x.gif"),
        { QStringLiteral(".klipy.com") }));
}

void GifResponseParserTest::klipyResultFalseIsMalformed()
{
    QVERIFY(!gif::parseKlipy("{\"result\":false,\"data\":{}}", Rating::R, 0).ok);
    QVERIFY(!gif::parseKlipy("not json", Rating::R, 0).ok);
    QVERIFY(!gif::parseKlipy("{\"result\":true,\"data\":{}}", Rating::R, 0).ok);
}

void GifResponseParserTest::klipyPageAdvancesAndHasNext()
{
    const auto more = gif::parseKlipy(klipyResponse(klipyItem("1"), true),
                                      Rating::R, 2);
    QCOMPARE(more.nextPage, 3);
    QVERIFY(more.hasMore);
    const auto last = gif::parseKlipy(klipyResponse(klipyItem("1"), false),
                                      Rating::R, 0);
    QVERIFY(!last.hasMore);
}

void GifResponseParserTest::klipySkipsNonGifType()
{
    QByteArray sticker =
        "{\"id\":5,\"type\":\"sticker\",\"file\":{\"md\":{\"gif\":{"
        "\"url\":\"https://static.klipy.com/x.gif\",\"width\":10,\"height\":10,"
        "\"size\":1}}}}";
    const auto out = gif::parseKlipy(klipyResponse(sticker), Rating::R, 0);
    QVERIFY(out.ok);
    QVERIFY(out.results.isEmpty());
}

void GifResponseParserTest::resultsCarryProviderIdentity()
{
    const auto g = gif::parseGiphy(giphyResponse(gifItem("g1", "g")), Rating::R, 0);
    const auto k = gif::parseKlipy(klipyResponse(klipyItem("2")), Rating::R, 0);
    QCOMPARE(g.results.first().provider, QStringLiteral("giphy"));
    QCOMPARE(k.results.first().provider, QStringLiteral("klipy"));
}

QTEST_GUILESS_MAIN(GifResponseParserTest)
#include "GifResponseParserTest.moc"
