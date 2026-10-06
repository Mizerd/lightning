// Rasterize on send (src/media/SvgRaster.h, SvgRasterJob.h).
//
// A picked SVG is converted LOCALLY to a PNG before anything is uploaded. What
// this pins:
//   * the size rules: declared size with units, viewBox-only files, a floor so
//     a tiny icon is drawn crisp at the target size (not upscaled), a ceiling
//     so `width="100000"` cannot ask for a gigantic PNG, and the aspect ratio
//     kept to within a pixel, extreme ratios included;
//   * the screen over hostile files: entities, gzip, oversized input, external
//     references, nesting, element and <use> counts, text where there is no
//     font database;
//   * the helper process: success, a helper's own refusal, a crash, a bad
//     output, and above all the TIMEOUT KILL, proved by the helper's pid being
//     gone afterwards;
//   * the real binary's hidden `--rasterize-svg` flag, which is how the app
//     runs all of the above.
//
// Not exercised: the GUI flows around it (picker, crop dialog, composer tray);
// those are in their own suites and the live check.

#include "media/SvgRasterJob.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#if defined(LIGHTNING_HAVE_QT_SVG)
#include <QSvgRenderer>
#endif
#include <QProcess>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/types.h>
#endif

namespace svg = lightning::svgraster;

namespace {

const char *kNs = "xmlns=\"http://www.w3.org/2000/svg\" "
                  "xmlns:xlink=\"http://www.w3.org/1999/xlink\"";

QByteArray doc(const QByteArray &attributes, const QByteArray &body)
{
    return QByteArray("<svg ") + kNs + " " + attributes + ">" + body + "</svg>";
}

QImage decode(const QByteArray &png)
{
    return QImage::fromData(png, "PNG");
}

// Pixels in row `y` whose alpha is neither fully clear nor fully opaque: the
// width of an antialiased edge.
int partialAlphaInRow(const QImage &image, int y)
{
    int partial = 0;
    for (int x = 0; x < image.width(); ++x) {
        const int a = qAlpha(image.pixel(x, y));
        if (a > 0 && a < 255)
            ++partial;
    }
    return partial;
}

// Helper-process doubles. `sh -c SCRIPT $0 $1...`: %OUT lands in $0.
struct ShHelper {
    QString program = QStandardPaths::findExecutable(QStringLiteral("sh"));
    bool ok() const { return !program.isEmpty(); }
};

} // namespace

class SvgRasterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init() { svg::hooks() = svg::Hooks{}; }
    void cleanup() { svg::hooks() = svg::Hooks{}; }

    // ---- shape detection ----------------------------------------------------

    void svgIsRecognisedByShapeNotByName()
    {
        QVERIFY(svg::looksLikeSvg("<svg/>"));
        QVERIFY(svg::looksLikeSvg("\xEF\xBB\xBF  \n<?xml version=\"1.0\"?><svg/>"));
        QVERIFY(!svg::looksLikeSvg("\x89PNG\r\n"));
        QVERIFY(!svg::looksLikeSvg("GIF89a"));
        QVERIFY(!svg::looksLikeSvg(QByteArray()));
        QVERIFY(svg::isGzip(QByteArray("\x1f\x8b\x08", 3)));
        QVERIFY(!svg::isGzip("<svg/>"));
        QVERIFY(svg::isSvgMime(QStringLiteral("image/svg+xml")));
        QVERIFY(!svg::isSvgMime(QStringLiteral("image/png")));
    }

    // ---- the declared size ----------------------------------------------------

    void unitsConvertAtNinetySixDpi()
    {
        const auto size = [](const char *w, const char *h) {
            return svg::declaredSize(
                doc(QByteArray("width=\"") + w + "\" height=\"" + h + "\"", {}));
        };
        QCOMPARE(size("100", "50"), QSizeF(100, 50));
        QCOMPARE(size("100px", "50px"), QSizeF(100, 50));
        QCOMPARE(size("72pt", "36pt"), QSizeF(96, 48));
        QCOMPARE(size("6pc", "3pc"), QSizeF(96, 48));
        QCOMPARE(size("1in", "2in"), QSizeF(96, 192));
        QCOMPARE(size("2em", "1em"), QSizeF(32, 16));
        QCOMPARE(size("4ex", "2ex"), QSizeF(32, 16));
        const QSizeF a4 = size("210mm", "297mm");
        QVERIFY(qAbs(a4.width() - 793.7008) < 0.01);
        QVERIFY(qAbs(a4.height() - 1122.5197) < 0.01);
        const QSizeF cm = size("2.54cm", "5.08cm");
        QVERIFY(qAbs(cm.width() - 96.0) < 0.001);
        QVERIFY(qAbs(cm.height() - 192.0) < 0.001);
        // An exponent is not a unit, and `em` is not an exponent.
        QCOMPARE(size("1e2", "5e1"), QSizeF(100, 50));
    }

    void aViewBoxAloneOrBesideAPartialSizeDecidesTheShape()
    {
        QCOMPARE(svg::declaredSize(doc("viewBox=\"0 0 300 100\"", {})),
                 QSizeF(300, 100));
        QCOMPARE(svg::declaredSize(doc("viewBox=\"10,20,640,480\"", {})),
                 QSizeF(640, 480));
        // One dimension, the shape from the viewBox.
        QCOMPARE(svg::declaredSize(doc("width=\"600\" viewBox=\"0 0 300 100\"", {})),
                 QSizeF(600, 200));
        QCOMPARE(svg::declaredSize(doc("height=\"50\" viewBox=\"0 0 300 100\"", {})),
                 QSizeF(150, 50));
        // A percentage declares nothing.
        QCOMPARE(svg::declaredSize(
                     doc("width=\"100%\" height=\"100%\" viewBox=\"0 0 30 20\"", {})),
                 QSizeF(30, 20));
        // Nothing at all, or nonsense.
        QVERIFY(!svg::declaredSize(doc("", {})).isValid());
        QVERIFY(!svg::declaredSize(doc("width=\"abc\" height=\"-4\"", {})).isValid());
        QVERIFY(!svg::declaredSize(doc("viewBox=\"0 0 0 10\"", {})).isValid());
    }

    // ---- the target size ------------------------------------------------------

    void aTinyIconIsDrawnAtTheFloorAndNeverBelowIt()
    {
        QCOMPARE(svg::targetSize(QSizeF(24, 24), svg::attachmentPolicy()),
                 QSize(1024, 1024));
        QCOMPARE(svg::targetSize(QSizeF(24, 12), svg::attachmentPolicy()),
                 QSize(1024, 512));
        QCOMPARE(svg::targetSize(QSizeF(24, 24), svg::cropSourcePolicy()),
                 QSize(1024, 1024));
        QCOMPARE(svg::targetSize(QSizeF(24, 24), svg::backgroundPolicy()),
                 QSize(2560, 2560));
        QCOMPARE(svg::targetSize(QSizeF(24, 24), svg::stickerPolicy()),
                 QSize(512, 512));
    }

    void aLargeDocumentKeepsItsOwnSizeUpToTheCeiling()
    {
        QCOMPARE(svg::targetSize(QSizeF(1600, 1200), svg::attachmentPolicy()),
                 QSize(1600, 1200));
        QCOMPARE(svg::targetSize(QSizeF(8000, 4000), svg::attachmentPolicy()),
                 QSize(4096, 2048));
        QCOMPARE(svg::targetSize(QSizeF(3000, 3000), svg::cropSourcePolicy()),
                 QSize(2048, 2048));
        QCOMPARE(svg::targetSize(QSizeF(100, 100), svg::backgroundPolicy()),
                 QSize(2560, 2560));
    }

    void aHugeDeclaredSizeIsClampedAndNothingExceedsTheAbsoluteCeiling()
    {
        const QSize huge =
            svg::targetSize(QSizeF(100000, 50000), { 1, 1000000 });
        QCOMPARE(huge, QSize(4096, 2048));
        const QSize wild = svg::targetSize(QSizeF(1e12, 1e12), { 50000, 90000 });
        QVERIFY(wild.width() <= svg::kAbsoluteMaxEdge);
        QVERIFY(wild.height() <= svg::kAbsoluteMaxEdge);
        // Not a size.
        QVERIFY(!svg::targetSize(QSizeF(0, 10), svg::attachmentPolicy()).isValid());
        QVERIFY(!svg::targetSize(QSizeF(qQNaN(), 10), svg::attachmentPolicy()).isValid());
        QVERIFY(!svg::targetSize(QSizeF(qInf(), 10), svg::attachmentPolicy()).isValid());
    }

    void extremeAspectRatiosKeepAtLeastOnePixelAndTheirShape()
    {
        QCOMPARE(svg::targetSize(QSizeF(10000, 10), svg::attachmentPolicy()),
                 QSize(4096, 4));
        QCOMPARE(svg::targetSize(QSizeF(10, 10000), svg::attachmentPolicy()),
                 QSize(4, 4096));
        QCOMPARE(svg::targetSize(QSizeF(1, 100000), svg::attachmentPolicy()),
                 QSize(1, 4096));
        QCOMPARE(svg::targetSize(QSizeF(100, 0.1), svg::attachmentPolicy()),
                 QSize(1024, 1));
    }

    void theAspectRatioIsKeptToWithinOnePixel()
    {
        const QList<QSizeF> shapes = {
            { 640, 480 }, { 333, 777 }, { 1920, 1080 }, { 50, 3 }, { 7, 900 },
            { 24, 24 }, { 3, 1 }, { 1013, 1019 }, { 5000, 4999 },
        };
        const svg::Policy policies[] = {
            svg::attachmentPolicy(), svg::cropSourcePolicy(),
            svg::backgroundPolicy(), svg::stickerPolicy(),
        };
        int checked = 0;
        for (const svg::Policy &policy : policies) {
            for (const QSizeF &shape : shapes) {
                const QSize out = svg::targetSize(shape, policy);
                QVERIFY(out.isValid());
                const double longest = qMax(shape.width(), shape.height());
                const int outLongest = qMax(out.width(), out.height());
                // The scale actually applied, from the longest side.
                const double scale = outLongest / longest;
                QVERIFY2(qAbs(out.width() - shape.width() * scale) <= 1.0,
                         qPrintable(QStringLiteral("%1x%2 -> %3x%4")
                                        .arg(shape.width()).arg(shape.height())
                                        .arg(out.width()).arg(out.height())));
                QVERIFY2(qAbs(out.height() - shape.height() * scale) <= 1.0,
                         qPrintable(QStringLiteral("%1x%2 -> %3x%4")
                                        .arg(shape.width()).arg(shape.height())
                                        .arg(out.width()).arg(out.height())));
                ++checked;
            }
        }
        QCOMPARE(checked, 4 * 9);
    }

    // ---- the render -------------------------------------------------------------

    void aViewBoxOnlyFileFillsItsCanvasWithoutStretchOrLetterbox()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        const auto result = svg::rasterize(
            doc("viewBox=\"0 0 300 100\"",
                "<rect width=\"300\" height=\"100\" fill=\"#0000ff\"/>"),
            svg::attachmentPolicy());
        QCOMPARE(result.refusal, QString());
        QCOMPARE(result.size, QSize(1024, 341));
        const QImage image = decode(result.png);
        QCOMPARE(image.size(), result.size);
        // Opaque to every corner: nothing letterboxed, nothing stretched away.
        QCOMPARE(image.pixelColor(0, 0), QColor(0, 0, 255));
        QCOMPARE(image.pixelColor(1023, 340), QColor(0, 0, 255));
        QCOMPARE(image.pixelColor(512, 170), QColor(0, 0, 255));
    }

    void millimetreUnitsGiveTheRightPixelSizeAndANonEmptyPicture()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        const auto result = svg::rasterize(
            doc("width=\"210mm\" height=\"297mm\"",
                "<rect width=\"793\" height=\"1122\" fill=\"#ff0000\"/>"),
            svg::attachmentPolicy());
        QCOMPARE(result.refusal, QString());
        // 793.7 x 1122.5 CSS px at 96 dpi, within a pixel.
        QVERIFY(qAbs(result.size.width() - 794) <= 1);
        QVERIFY(qAbs(result.size.height() - 1123) <= 1);
        const QImage image = decode(result.png);
        QVERIFY(qAlpha(image.pixel(result.size.width() / 2,
                                    result.size.height() / 2)) == 255);
    }

    // A 24x24 icon is DRAWN at 1024, so its edge is as sharp as the vector, not
    // the ~43 px smear of a small raster scaled up.
    void aTinyIconIsCrispNotAnUpscaledRaster()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        const QByteArray icon = doc(
            "width=\"24\" height=\"24\" viewBox=\"0 0 24 24\"",
            "<rect x=\"0\" y=\"0\" width=\"12\" height=\"24\" fill=\"#000\"/>");
        const auto result = svg::rasterize(icon, svg::attachmentPolicy());
        QCOMPARE(result.refusal, QString());
        QCOMPARE(result.size, QSize(1024, 1024));
        const QImage crisp = decode(result.png);
        QCOMPARE(crisp.size(), QSize(1024, 1024));
        const int crispEdge = partialAlphaInRow(crisp, 512);
        QVERIFY2(crispEdge <= 2,
                 qPrintable(QStringLiteral("edge is %1 px wide").arg(crispEdge)));
        QCOMPARE(qAlpha(crisp.pixel(100, 512)), 255);
        QCOMPARE(qAlpha(crisp.pixel(900, 512)), 0);

#if defined(LIGHTNING_HAVE_QT_SVG)
        // The reference render: the same renderer drawn straight to 1024.
        QSvgRenderer renderer;
        QVERIFY(renderer.load(icon));
        QImage reference(1024, 1024, QImage::Format_ARGB32_Premultiplied);
        reference.fill(Qt::transparent);
        {
            QPainter painter(&reference);
            renderer.render(&painter, QRectF(0, 0, 1024, 1024));
        }
        QCOMPARE(partialAlphaInRow(reference, 512), crispEdge);

        // The failure this guards: render at the icon's own size, then scale.
        QImage small(24, 24, QImage::Format_ARGB32_Premultiplied);
        small.fill(Qt::transparent);
        {
            QPainter painter(&small);
            renderer.render(&painter, QRectF(0, 0, 24, 24));
        }
        const QImage blurry = small.scaled(1024, 1024, Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);
        QVERIFY2(partialAlphaInRow(blurry, 512) > 10,
                 "the upscaled-raster control should smear the edge, or this "
                 "test cannot tell the two apart");
#endif
    }

    void aHugeDeclaredSizeProducesABoundedPngWithTheSameShape()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        const auto result = svg::rasterize(
            doc("width=\"100000\" height=\"50000\"",
                "<rect width=\"100000\" height=\"50000\" fill=\"#00ff00\"/>"),
            svg::attachmentPolicy());
        QCOMPARE(result.refusal, QString());
        QCOMPARE(result.size, QSize(4096, 2048));
        QVERIFY(result.png.size() < svg::kMaxPngBytes);
        QCOMPARE(decode(result.png).size(), QSize(4096, 2048));
    }

    void anExtremeAspectRatioRenders()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        const auto wide = svg::rasterize(
            doc("viewBox=\"0 0 10000 10\"",
                "<rect width=\"10000\" height=\"10\"/>"),
            svg::attachmentPolicy());
        QCOMPARE(wide.refusal, QString());
        QCOMPARE(wide.size, QSize(4096, 4));
        const auto tall = svg::rasterize(
            doc("viewBox=\"0 0 1 100000\"", "<rect width=\"1\" height=\"100000\"/>"),
            svg::attachmentPolicy());
        QCOMPARE(tall.refusal, QString());
        QCOMPARE(tall.size, QSize(1, 4096));
    }

    void theBackgroundAndStickerPoliciesApplyToTheRender()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        const QByteArray square = doc("width=\"24\" height=\"24\"",
                                      "<rect width=\"24\" height=\"24\"/>");
        QCOMPARE(svg::rasterize(square, svg::backgroundPolicy()).size,
                 QSize(2560, 2560));
        QCOMPARE(svg::rasterize(square, svg::stickerPolicy()).size,
                 QSize(512, 512));
        QCOMPARE(svg::rasterize(square, svg::cropSourcePolicy()).size,
                 QSize(1024, 1024));
    }

    void theBackgroundIsTransparent()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        const auto result = svg::rasterize(
            doc("viewBox=\"0 0 10 10\"",
                "<circle cx=\"5\" cy=\"5\" r=\"2\" fill=\"#fff\"/>"),
            svg::attachmentPolicy());
        QCOMPARE(result.refusal, QString());
        const QImage image = decode(result.png);
        QVERIFY(image.hasAlphaChannel());
        QCOMPARE(qAlpha(image.pixel(2, 2)), 0);
        QCOMPARE(qAlpha(image.pixel(512, 512)), 255);
    }

    // ---- the screen over hostile files --------------------------------------------

    void hostileFilesAreRefusedBeforeTheRenderer()
    {
        const QByteArray ten = "<!DOCTYPE svg [<!ENTITY a \"aaaaaaaaaa\">"
                               "<!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">"
                               "<!ENTITY c \"&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;\">"
                               "<!ENTITY d \"&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;\">"
                               "<!ENTITY e \"&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;\">]>";
        QByteArray laughs = ten + doc("width=\"1\" height=\"1\"",
                                      "<desc>&e;&e;&e;&e;&e;&e;&e;&e;&e;&e;</desc>");
        // A billion-laughs shape: refused in some way, never expanded, and fast.
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!svg::screen(laughs).isEmpty());
        QVERIFY2(timer.elapsed() < 3000, "the entity bomb was expanded");
        const auto bomb = svg::rasterize(laughs, svg::attachmentPolicy());
        QVERIFY(bomb.png.isEmpty());
        QVERIFY(!bomb.refusal.isEmpty());

        // Entities that look small but reference past the budget.
        QByteArray refs = QByteArray("<!DOCTYPE svg [<!ENTITY e \"")
            + QByteArray(4000, 'a') + QByteArray("\">]>")
            + doc("", "<desc t=\"" + QByteArray("&e;").repeated(1000) + "\"/>");
        QCOMPARE(svg::screen(refs), QStringLiteral("entities"));

        // An external entity.
        QCOMPARE(svg::screen(QByteArray("<?xml version=\"1.0\"?><!DOCTYPE svg ["
                                        "<!ENTITY x SYSTEM \"file:///etc/passwd\">]>")
                             + doc("", "<text>&x;</text>")),
                 QStringLiteral("entities"));
    }

    void anSvgzAndAGzipBombAreRefusedWithoutBeingInflated()
    {
        // Real gzip magic followed by a megabyte: the 1 MB is never read as
        // anything, because the first two bytes decide.
        const QByteArray gz = QByteArray("\x1f\x8b\x08\x00", 4)
            + QByteArray(1024 * 1024, '\0');
        QCOMPARE(svg::screen(gz), QStringLiteral("compressed"));
        const auto result = svg::rasterize(gz, svg::attachmentPolicy());
        QCOMPARE(result.refusal, QStringLiteral("compressed"));
        QVERIFY(result.png.isEmpty());
    }

    void oversizedInputIsRefused()
    {
        QCOMPARE(svg::screen(QByteArray(svg::kMaxSourceBytes + 1, ' ')),
                 QStringLiteral("too_large"));
        QCOMPARE(svg::kMaxSourceBytes, qsizetype(2 * 1024 * 1024));
        // Exactly the limit is not "too large" (it is just not an SVG).
        QVERIFY(svg::screen(QByteArray(svg::kMaxSourceBytes, ' '))
                != QStringLiteral("too_large"));
    }

    void externalReferencesAreRefused()
    {
        QCOMPARE(svg::screen(doc("", "<image href=\"/home/u/private.png\"/>")),
                 QStringLiteral("external_image"));
        QCOMPARE(svg::screen(doc("", "<image xlink:href=\"data:image/png;base64,AAAA\"/>")),
                 QStringLiteral("external_image"));
        QCOMPARE(svg::screen(doc("", "<filter id=\"f\"><feImage href=\"x.png\"/></filter>")),
                 QStringLiteral("external_image"));
        QCOMPARE(svg::screen(doc("", "<use xlink:href=\"http://evil.example/a.svg#x\"/>")),
                 QStringLiteral("external_reference"));
        QCOMPARE(svg::screen(doc("", "<rect fill=\"url(http://evil.example/p)\"/>")),
                 QStringLiteral("external_reference"));
        QCOMPARE(svg::screen(doc("", "<style>@import url(x.css);</style>")),
                 QStringLiteral("external_reference"));
        // Document-internal references are fine.
        QCOMPARE(svg::screen(doc("", "<defs><linearGradient id=\"g\"/></defs>"
                                     "<rect fill=\"url(#g)\"/><use xlink:href=\"#g\"/>")),
                 QString());
    }

    void deepAndHugeDocumentsAreRefused()
    {
        QCOMPARE(svg::screen(doc("", QByteArray("<g>").repeated(svg::kMaxDepth)
                                      + QByteArray("</g>").repeated(svg::kMaxDepth))),
                 QStringLiteral("too_deep"));
        QCOMPARE(svg::screen(doc("", QByteArray("<rect/>").repeated(svg::kMaxElements))),
                 QStringLiteral("too_many_elements"));
        QCOMPARE(svg::screen(doc("", QByteArray("<use xlink:href=\"#a\"/>")
                                         .repeated(svg::kMaxUseElements + 1))),
                 QStringLiteral("too_many_elements"));
    }

    void textIsRefusedWhereThereIsNoFontDatabase()
    {
        const QByteArray withText = doc("", "<text x=\"1\" y=\"9\">Hi</text>");
        QCOMPARE(svg::screen(withText, false), QStringLiteral("text"));
        QCOMPARE(svg::screen(withText, true), QString());
        QCOMPARE(svg::screen(doc("", "<g><tspan>x</tspan></g>"), false),
                 QStringLiteral("text"));
        QCOMPARE(svg::screen(doc("", "<rect/>"), false), QString());
    }

    void everyRefusalHasAHumanSentenceAndNeverTheRawCode()
    {
        const QStringList codes = {
            QStringLiteral("too_large"), QStringLiteral("compressed"),
            QStringLiteral("too_many_elements"), QStringLiteral("too_deep"),
            QStringLiteral("entities"), QStringLiteral("timeout"),
            QStringLiteral("external_image"), QStringLiteral("external_reference"),
            QStringLiteral("text"), QStringLiteral("unavailable"),
            QStringLiteral("empty"), QStringLiteral("not_svg"),
            QStringLiteral("malformed"), QStringLiteral("unrenderable"),
            QStringLiteral("crashed"), QStringLiteral("helper_failed"),
            QStringLiteral("never_heard_of_it"),
        };
        for (const QString &code : codes) {
            const QString text = svg::userMessage(code);
            QVERIFY(text.size() > 20);
            QVERIFY2(!text.contains(code) || code == QLatin1String("text"),
                     qPrintable(code));
        }
        QVERIFY(svg::userMessage(QStringLiteral("timeout"))
                    .contains(QStringLiteral("too complex")));
    }

    // ---- the job runner ----------------------------------------------------------

    void theCallbackIsAsynchronousAndCarriesThePng()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG (LIGHTNING_HAVE_QT_SVG unset)");
        svg::hooks().inProcess = true;
        QObject context;
        bool done = false;
        svg::Result got;
        svg::rasterizeAsync(
            doc("viewBox=\"0 0 40 20\"", "<rect width=\"40\" height=\"20\"/>"),
            svg::attachmentPolicy(), &context, [&](const svg::Result &r) {
                got = r;
                done = true;
            });
        QVERIFY2(!done, "the callback ran inside the call");
        QTRY_VERIFY(done);
        QCOMPARE(got.refusal, QString());
        QCOMPARE(got.size, QSize(1024, 512));
    }

    void aPlainlyBadFileNeverCostsAProcess()
    {
        // No helper configured: if one were started it would be the test
        // binary itself with a flag it does not know, and fail differently.
        QObject context;
        QString refusal;
        bool done = false;
        svg::rasterizeAsync(doc("", "<image href=\"/etc/hostname\"/>"),
                            svg::attachmentPolicy(), &context,
                            [&](const svg::Result &r) {
                                refusal = r.refusal;
                                done = true;
                            });
        QTRY_VERIFY(done);
        QCOMPARE(refusal, QStringLiteral("external_image"));
    }

    void aCallbackForADeadRequesterIsNeverRun()
    {
        svg::hooks().inProcess = true;
        bool ran = false;
        {
            QObject context;
            svg::rasterizeAsync(doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                                svg::attachmentPolicy(), &context,
                                [&](const svg::Result &) { ran = true; });
        }
        QTest::qWait(300);
        QVERIFY(!ran);
    }

#ifdef Q_OS_UNIX
    // THE TIMEOUT KILLS THE HELPER. The pid is read from the helper's own
    // write, and afterwards no process with it exists.
    void aHelperPastItsTimeoutIsKilled()
    {
        ShHelper sh;
        if (!sh.ok())
            QSKIP("no sh");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pidFile = dir.filePath(QStringLiteral("pid"));
        svg::hooks().program = sh.program;
        svg::hooks().args = { QStringLiteral("-c"),
                              QStringLiteral("echo $$ > \"$1\"; exec sleep 60"),
                              QStringLiteral("x"), pidFile };
        svg::hooks().timeoutMs = 400;

        QObject context;
        QString refusal;
        bool done = false;
        QElapsedTimer timer;
        timer.start();
        svg::rasterizeAsync(doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                            svg::attachmentPolicy(), &context,
                            [&](const svg::Result &r) {
                                refusal = r.refusal;
                                done = true;
                            });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        if (!svg::available())
            QSKIP("built without Qt SVG: the job refuses before spawning");
        QCOMPARE(refusal, QStringLiteral("timeout"));
        QVERIFY2(timer.elapsed() < 8000, "the kill was not prompt");

        QFile file(pidFile);
        QVERIFY2(file.open(QIODevice::ReadOnly), "the helper never started");
        const pid_t pid = file.readAll().trimmed().toInt();
        QVERIFY(pid > 1);
        // Reaped by QProcess; allow the kernel a moment.
        bool gone = false;
        for (int i = 0; i < 50 && !gone; ++i) {
            gone = ::kill(pid, 0) != 0;
            if (!gone)
                QTest::qWait(20);
        }
        QVERIFY2(gone, "the helper is still running after the timeout");
    }

    void aHelperThatCrashesIsReportedAsCrashed()
    {
        ShHelper sh;
        if (!sh.ok() || !svg::available())
            QSKIP("no sh, or no Qt SVG");
        svg::hooks().program = sh.program;
        svg::hooks().args = { QStringLiteral("-c"), QStringLiteral("kill -SEGV $$") };
        QObject context;
        QString refusal;
        bool done = false;
        svg::rasterizeAsync(doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                            svg::attachmentPolicy(), &context,
                            [&](const svg::Result &r) {
                                refusal = r.refusal;
                                done = true;
                            });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QCOMPARE(refusal, QStringLiteral("crashed"));
    }

    // A helper whose GUI app cannot start crashes; the job asks once more with
    // --no-text and the second answer is delivered. A helper that crashes both
    // times is reported as crashed, after exactly two starts.
    void aCrashedHelperIsRetriedOnceWithNoText()
    {
        ShHelper sh;
        if (!sh.ok() || !svg::available())
            QSKIP("no sh, or no Qt SVG");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage fine(30, 20, QImage::Format_ARGB32);
        fine.fill(Qt::blue);
        const QString okPng = dir.filePath(QStringLiteral("ok.png"));
        QVERIFY(fine.save(okPng, "PNG"));
        const QString runs = dir.filePath(QStringLiteral("runs"));

        const auto run = [&](const QString &script) {
            QFile::remove(runs);
            svg::hooks().program = sh.program;
            svg::hooks().args = { QStringLiteral("-c"), script,
                                  QStringLiteral("%OUT"), okPng, runs };
            QObject context;
            svg::Result got;
            bool done = false;
            svg::rasterizeAsync(
                doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                svg::attachmentPolicy(), &context, [&](const svg::Result &r) {
                    got = r;
                    done = true;
                });
            [&] { QTRY_VERIFY_WITH_TIMEOUT(done, 15000); }();
            return got;
        };
        const auto starts = [&] {
            QFile f(runs);
            return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
        };

        // $0=%OUT $1=png $2=runs, then the optional --no-text as $3.
        const svg::Result healed = run(QStringLiteral(
            "echo x >> \"$2\"; if [ \"$3\" = --no-text ]; then cp \"$1\" \"$0\"; "
            "else kill -SEGV $$; fi"));
        QCOMPARE(healed.refusal, QString());
        QCOMPARE(healed.size, QSize(30, 20));
        QCOMPARE(starts(), 2);

        const svg::Result broken = run(QStringLiteral(
            "echo x >> \"$2\"; kill -SEGV $$"));
        QCOMPARE(broken.refusal, QStringLiteral("crashed"));
        QCOMPARE(starts(), 2);
    }

    void aHelpersOwnRefusalComesThroughWithItsReason()
    {
        ShHelper sh;
        if (!sh.ok() || !svg::available())
            QSKIP("no sh, or no Qt SVG");
        svg::hooks().program = sh.program;
        svg::hooks().args = { QStringLiteral("-c"),
                              QStringLiteral("printf too_deep > \"$0\"; exit 3"),
                              QStringLiteral("%OUT") };
        QObject context;
        QString refusal;
        bool done = false;
        svg::rasterizeAsync(doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                            svg::attachmentPolicy(), &context,
                            [&](const svg::Result &r) {
                                refusal = r.refusal;
                                done = true;
                            });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QCOMPARE(refusal, QStringLiteral("too_deep"));
    }

    // The helper's output is not trusted either: a PNG over the ceiling, or
    // not a PNG at all, is refused rather than uploaded.
    void aHelperOutputThatIsNotAnAcceptablePngIsRefused()
    {
        ShHelper sh;
        if (!sh.ok() || !svg::available())
            QSKIP("no sh, or no Qt SVG");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QImage tooWide(5000, 1, QImage::Format_ARGB32);
        tooWide.fill(Qt::red);
        const QString widePng = dir.filePath(QStringLiteral("wide.png"));
        QVERIFY(tooWide.save(widePng, "PNG"));
        const QString junk = dir.filePath(QStringLiteral("junk.png"));
        {
            QFile f(junk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("<svg xmlns='http://www.w3.org/2000/svg'/>");
        }
        QImage fine(30, 20, QImage::Format_ARGB32);
        fine.fill(Qt::blue);
        const QString okPng = dir.filePath(QStringLiteral("ok.png"));
        QVERIFY(fine.save(okPng, "PNG"));

        const auto run = [&](const QString &source) {
            svg::hooks().program = sh.program;
            svg::hooks().args = { QStringLiteral("-c"),
                                  QStringLiteral("cp \"$1\" \"$0\""),
                                  QStringLiteral("%OUT"), source };
            QObject context;
            svg::Result got;
            bool done = false;
            svg::rasterizeAsync(
                doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                svg::attachmentPolicy(), &context, [&](const svg::Result &r) {
                    got = r;
                    done = true;
                });
            [&] { QTRY_VERIFY_WITH_TIMEOUT(done, 10000); }();
            return got;
        };
        QCOMPARE(run(widePng).refusal, QStringLiteral("helper_failed"));
        QCOMPARE(run(junk).refusal, QStringLiteral("helper_failed"));
        const svg::Result good = run(okPng);
        QCOMPARE(good.refusal, QString());
        QCOMPARE(good.size, QSize(30, 20));
    }

    void aHelperThatCannotStartIsReported()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG");
        svg::hooks().program = QStringLiteral("/nonexistent/lightning-helper");
        svg::hooks().args = { QStringLiteral("x") };
        QObject context;
        QString refusal;
        bool done = false;
        svg::rasterizeAsync(doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                            svg::attachmentPolicy(), &context,
                            [&](const svg::Result &r) {
                                refusal = r.refusal;
                                done = true;
                            });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QCOMPARE(refusal, QStringLiteral("helper_failed"));
    }

    // At most three helpers run at once; the rest queue and all complete.
    void manyRequestsAllCompleteAndNeverRunMoreThanTheLimit()
    {
        ShHelper sh;
        if (!sh.ok() || !svg::available())
            QSKIP("no sh, or no Qt SVG");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage fine(8, 8, QImage::Format_ARGB32);
        fine.fill(Qt::green);
        const QString okPng = dir.filePath(QStringLiteral("ok.png"));
        QVERIFY(fine.save(okPng, "PNG"));
        // Each helper records the number of its siblings alive when it began.
        const QString log = dir.filePath(QStringLiteral("log"));
        svg::hooks().program = sh.program;
        svg::hooks().args = {
            QStringLiteral("-c"),
            QStringLiteral("echo start >> \"$2\"; sleep 0.4; cp \"$1\" \"$0\"; "
                           "echo end >> \"$2\""),
            QStringLiteral("%OUT"), okPng, log };
        QObject context;
        int completed = 0;
        for (int i = 0; i < 8; ++i) {
            svg::rasterizeAsync(
                doc("viewBox=\"0 0 4 4\"", "<rect width=\"4\" height=\"4\"/>"),
                svg::attachmentPolicy(), &context,
                [&](const svg::Result &r) {
                    if (r.refusal.isEmpty())
                        ++completed;
                });
        }
        QTRY_COMPARE_WITH_TIMEOUT(completed, 8, 30000);
        QFile f(log);
        QVERIFY(f.open(QIODevice::ReadOnly));
        int alive = 0, peak = 0;
        for (const QByteArray &line : f.readAll().split('\n')) {
            if (line == "start")
                peak = qMax(peak, ++alive);
            else if (line == "end")
                --alive;
        }
        QVERIFY2(peak <= svg::kMaxParallel,
                 qPrintable(QStringLiteral("peak %1").arg(peak)));
        QVERIFY(peak >= 2); // they did overlap, so the limit was exercised
    }
#endif

    // ---- the real binary ----------------------------------------------------------

    // The hidden flag is registered in the preflight pass, exits, and speaks the
    // protocol: PNG on success, reason code and exit 3 on refusal.
    void theRealBinaryConvertsAndRefusesThroughItsHiddenFlag()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG");
        const QString binary = QStringLiteral(LIGHTNING_BINARY);
        if (!QFileInfo::exists(binary))
            QSKIP("the app binary is not built");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto run = [&](const QByteArray &input, int *exitCode) {
            const QString in = dir.filePath(QStringLiteral("in.svg"));
            const QString out = dir.filePath(QStringLiteral("out.bin"));
            QFile::remove(out);
            QFile f(in);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.write(input);
            f.close();
            QProcess process;
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start(binary, { QStringLiteral("--rasterize-svg"), in, out,
                                    QStringLiteral("1024"), QStringLiteral("4096") });
            if (!process.waitForFinished(30000)) {
                process.kill();
                process.waitForFinished();
                *exitCode = -99;
                return QByteArray();
            }
            *exitCode = process.exitCode();
            QFile r(out);
            return r.open(QIODevice::ReadOnly) ? r.readAll() : QByteArray();
        };

        int code = -1;
        const QByteArray png = run(
            doc("viewBox=\"0 0 30 10\"", "<rect width=\"30\" height=\"10\" fill=\"#f00\"/>"),
            &code);
        QCOMPARE(code, 0);
        QCOMPARE(decode(png).size(), QSize(1024, 341));

        const QByteArray reason = run(doc("", "<image href=\"/etc/hostname\"/>"), &code);
        QCOMPARE(code, 3);
        QCOMPARE(reason, QByteArray("external_image"));

        // Missing arguments are a usage error, not a launched application.
        QProcess usage;
        usage.start(binary, { QStringLiteral("--rasterize-svg"), QStringLiteral("only-one") });
        QVERIFY(usage.waitForFinished(30000));
        QCOMPARE(usage.exitCode(), 2);
    }

    // The point of the process: a file that would hold QtSvg for ever is killed
    // at the timeout and the app is none the worse.
    void aPathologicalFileIsKilledThroughTheRealHelper()
    {
        if (!svg::available())
            QSKIP("built without Qt SVG");
        const QString binary = QStringLiteral(LIGHTNING_BINARY);
        if (!QFileInfo::exists(binary))
            QSKIP("the app binary is not built");

        // Eight levels of ten <use>s: 10^8 draws from 90 elements.
        QByteArray defs = "<defs><path id=\"p\" d=\"M0 0h9v9z\"/>"
                          "<g id=\"g0\"><use xlink:href=\"#p\"/></g>";
        for (int level = 1; level <= 8; ++level) {
            defs += "<g id=\"g" + QByteArray::number(level) + "\">";
            for (int i = 0; i < 10; ++i)
                defs += "<use xlink:href=\"#g" + QByteArray::number(level - 1) + "\"/>";
            defs += "</g>";
        }
        defs += "</defs>";
        const QByteArray bomb =
            doc("width=\"64\" height=\"64\"", defs + "<use xlink:href=\"#g8\"/>");
        QCOMPARE(svg::screen(bomb), QString()); // it passes the screen: only the kill stops it

        svg::hooks().program = binary;
        svg::hooks().args = { QStringLiteral("--rasterize-svg"), QStringLiteral("%IN"),
                              QStringLiteral("%OUT"), QStringLiteral("%MIN"),
                              QStringLiteral("%MAX") };
        svg::hooks().timeoutMs = 1500;
        QObject context;
        svg::Result got;
        bool done = false;
        QElapsedTimer timer;
        timer.start();
        svg::rasterizeAsync(bomb, svg::attachmentPolicy(), &context,
                            [&](const svg::Result &r) {
                                got = r;
                                done = true;
                            });
        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        if (got.refusal.isEmpty())
            QSKIP("this Qt draws the nested <use> file quickly; nothing to kill");
        QCOMPARE(got.refusal, QStringLiteral("timeout"));
        QVERIFY2(timer.elapsed() < 15000, "the kill was not prompt");
    }
};

QTEST_GUILESS_MAIN(SvgRasterTest)
#include "SvgRasterTest.moc"
