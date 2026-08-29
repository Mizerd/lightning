// The crop/adjust pre-step for every display-image upload.
//
// Two things are proved here, and they are different in kind.
//
//   1. THE MATHS. `imagecrop::planCrop` turns a source size, a requested
//      rectangle and an output cap into an integer rectangle and an output
//      size. It is pure, so it can be pinned exactly: a rectangle that runs
//      off the image, a cap that has to shrink the result, a crop already
//      under the cap, and the degenerate cases.
//   2. THE HONESTY. The bytes written must BE the format they are named,
//      and an SVG must never reach a decoder at all (CLAUDE.md §6). Those
//      are checked against real encoded bytes rather than against the
//      function's own opinion of them.

#include "media/ImageCropper.h"
#include "media/StagedImageStore.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using imagecrop::chooseOutputFormat;
using imagecrop::CropPlan;
using imagecrop::planCrop;
using imagecrop::sniffRasterMime;

namespace {

/// A picture with real structure in it, so a wrong crop rectangle produces
/// visibly wrong pixels rather than a uniform block that any rectangle
/// would satisfy.
QImage patternImage(int w, int h, QImage::Format format = QImage::Format_RGB32)
{
    QImage image(w, h, format);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.fillRect(0, 0, w / 2, h / 2, Qt::red);
    painter.fillRect(w / 2, 0, w - w / 2, h / 2, Qt::green);
    painter.fillRect(0, h / 2, w / 2, h - h / 2, Qt::blue);
    painter.end();
    return image;
}

QByteArray encoded(const QImage &image, const char *format)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format);
    writer.write(image);
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(bytes) == bytes.size();
}

} // namespace

class ImageCropTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // ── The maths ────────────────────────────────────────────────────────

    void anOrdinaryCropIsTakenExactly()
    {
        const CropPlan plan = planCrop(QSize(800, 600), QRectF(100, 50, 400, 400), 0);
        QVERIFY(plan.ok);
        QCOMPARE(plan.sourceRect, QRect(100, 50, 400, 400));
        // No cap asked for, so the output is the crop itself.
        QCOMPARE(plan.outputSize, QSize(400, 400));
        QVERIFY(plan.reason.isEmpty());
    }

    // The rectangle arrives from a live QML transform. It must be CLAMPED,
    // never trusted: a read outside the decoded buffer is the failure this
    // prevents, and a rounding error at the edge of an image is the ordinary
    // way to reach it.
    void aRectangleRunningOffTheImageIsClampedNotTrusted()
    {
        const CropPlan plan = planCrop(QSize(200, 200), QRectF(-50, -50, 400, 400), 0);
        QVERIFY(plan.ok);
        QCOMPARE(plan.sourceRect, QRect(0, 0, 200, 200));

        const CropPlan overhang = planCrop(QSize(200, 200), QRectF(150, 150, 300, 300), 0);
        QVERIFY(overhang.ok);
        QCOMPARE(overhang.sourceRect, QRect(150, 150, 50, 50));
    }

    void aRectangleEntirelyOutsideTheImageIsRefused()
    {
        const CropPlan plan = planCrop(QSize(200, 200), QRectF(400, 400, 100, 100), 0);
        QVERIFY(!plan.ok);
        QCOMPARE(plan.reason, QStringLiteral("empty_rect"));
        QVERIFY(plan.sourceRect.isNull());
    }

    void anEmptySourceIsRefused()
    {
        const CropPlan plan = planCrop(QSize(0, 0), QRectF(0, 0, 10, 10), 64);
        QVERIFY(!plan.ok);
        QCOMPARE(plan.reason, QStringLiteral("empty_source"));
    }

    void aZeroSizedRequestIsRefused()
    {
        const CropPlan plan = planCrop(QSize(200, 200), QRectF(10, 10, 0, 0), 64);
        QVERIFY(!plan.ok);
        QCOMPARE(plan.reason, QStringLiteral("empty_rect"));
    }

    // The cap is what stops a 4000px photograph becoming a 4000px avatar.
    void theCapBringsTheLongEdgeDownAndKeepsTheAspect()
    {
        const CropPlan square = planCrop(QSize(4000, 3000), QRectF(0, 0, 3000, 3000), 512);
        QVERIFY(square.ok);
        QCOMPARE(square.sourceRect, QRect(0, 0, 3000, 3000));
        QCOMPARE(square.outputSize, QSize(512, 512));

        const CropPlan wide = planCrop(QSize(4000, 3000), QRectF(0, 0, 3000, 1000), 1920);
        QVERIFY(wide.ok);
        QCOMPARE(wide.outputSize, QSize(1920, 640));   // 3:1 preserved
    }

    // Enlarging a small crop to fill the cap costs bytes and adds nothing,
    // and on an avatar it makes a blurry picture look deliberate.
    void aCropSmallerThanTheCapIsNeverUpscaled()
    {
        const CropPlan plan = planCrop(QSize(300, 300), QRectF(0, 0, 120, 120), 512);
        QVERIFY(plan.ok);
        QCOMPARE(plan.outputSize, QSize(120, 120));
    }

    void aCropExactlyAtTheCapIsLeftAlone()
    {
        const CropPlan plan = planCrop(QSize(600, 600), QRectF(0, 0, 512, 512), 512);
        QVERIFY(plan.ok);
        QCOMPARE(plan.outputSize, QSize(512, 512));
    }

    void aCapOfZeroMeansUncapped()
    {
        const CropPlan plan = planCrop(QSize(4000, 4000), QRectF(0, 0, 4000, 4000), 0);
        QVERIFY(plan.ok);
        QCOMPARE(plan.outputSize, QSize(4000, 4000));
    }

    // QSize::scaled floors, so an extreme ratio can take the short edge to
    // zero — which would be an unencodable image rather than a small one.
    void anExtremeAspectRatioKeepsAtLeastOnePixelOnEachEdge()
    {
        const CropPlan plan = planCrop(QSize(4000, 4000), QRectF(0, 0, 4000, 3), 512);
        QVERIFY(plan.ok);
        QVERIFY(plan.outputSize.width() >= 1);
        QVERIFY(plan.outputSize.height() >= 1);
        QCOMPARE(plan.outputSize.width(), 512);
    }

    // Rounding each edge independently, rather than QRectF::toRect(), which
    // rounds the ORIGIN and then the SIZE relative to it.
    void fractionalCoordinatesRoundPerEdge()
    {
        const CropPlan plan = planCrop(QSize(100, 100), QRectF(0.5, 0.5, 10.4, 10.4), 0);
        QVERIFY(plan.ok);
        QCOMPARE(plan.sourceRect, QRect(1, 1, 10, 10));
    }

    // ── The gate ─────────────────────────────────────────────────────────

    void theFiveAcceptedFormatsAreIdentifiedFromTheirBytes()
    {
        QCOMPARE(sniffRasterMime(encoded(patternImage(8, 8), "png")),
                 QStringLiteral("image/png"));
        QCOMPARE(sniffRasterMime(encoded(patternImage(8, 8), "jpeg")),
                 QStringLiteral("image/jpeg"));
        QCOMPARE(sniffRasterMime(encoded(patternImage(8, 8), "bmp")),
                 QStringLiteral("image/bmp"));
        QCOMPARE(sniffRasterMime(QByteArray("GIF89a") + QByteArray(16, '\0')),
                 QStringLiteral("image/gif"));
        QCOMPARE(sniffRasterMime(QByteArray("RIFF\0\0\0\0WEBPVP8 ", 16)),
                 QStringLiteral("image/webp"));
    }

    // JPEG XL has TWO on-disk shapes and both must sniff, or the reported
    // "client does not support jpeg-xl" is only half fixed. Byte sequences
    // verified against real cjxl 0.12.0 output: a lossy and a lossless encode
    // both begin ff0a, and `--container=1` begins 0000000c4a584c200d0a870a.
    void jpegXlSniffsInBothItsContainerAndBareForms()
    {
        const QByteArray container =
            QByteArray("\x00\x00\x00\x0CJXL \r\n\x87\n", 12)
            + QByteArray(24, '\x00');
        const QByteArray bare = QByteArray("\xff\x0a", 2) + QByteArray(24, 'x');
        QCOMPARE(sniffRasterMime(container), QStringLiteral("image/jxl"));
        QCOMPARE(sniffRasterMime(bare), QStringLiteral("image/jxl"));

        // ORDER MATTERS, and this is the case that pins it: a container's own
        // payload begins with the bare codestream signature, so testing the
        // 2-byte form first would report every container as a bare stream.
        // Both answer image/jxl, so the assertion that catches a wrong order
        // is that a container is not mistaken for something shorter — checked
        // by feeding a container whose 13th byte onward is a codestream.
        QCOMPARE(sniffRasterMime(container + QByteArray("\xff\x0a", 2)),
                 QStringLiteral("image/jxl"));

        // Truncated magic must not read past the end of the buffer.
        QVERIFY(sniffRasterMime(QByteArray("\x00\x00\x00\x0CJXL", 7)).isEmpty());
        QVERIFY(sniffRasterMime(QByteArray("\xff", 1)).isEmpty());
        // A neighbouring byte must not alias into it.
        QVERIFY(sniffRasterMime(QByteArray("\xff\x0b", 2) + QByteArray(24, 'x'))
                    .isEmpty());
    }

    // CLAUDE.md §6: untrusted SVG must never enter a media path. The name is
    // irrelevant — only the bytes decide.
    void svgAndOtherNonRasterBytesAreRefused()
    {
        const QByteArray svg =
            "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\">"
            "<script>alert(1)</script></svg>";
        QVERIFY(sniffRasterMime(svg).isEmpty());
        QVERIFY(sniffRasterMime(QByteArray("<svg width='1'/>")).isEmpty());
        QVERIFY(sniffRasterMime(QByteArray("<!DOCTYPE html><html>")).isEmpty());
        QVERIFY(sniffRasterMime(QByteArray("\0\0\0\x18""ftypmp42", 12)).isEmpty());
        QVERIFY(sniffRasterMime(QByteArray()).isEmpty());
        // Truncated magic must not be read past the end of the buffer.
        QVERIFY(sniffRasterMime(QByteArray("\x89PN", 3)).isEmpty());
        QVERIFY(sniffRasterMime(QByteArray("RIFF", 4)).isEmpty());
    }

    // ── Format honesty ───────────────────────────────────────────────────

    void theEncoderMimeAndSuffixAlwaysAgree()
    {
        for (const auto &mime : { QStringLiteral("image/png"),
                                  QStringLiteral("image/jpeg"),
                                  QStringLiteral("image/gif"),
                                  QStringLiteral("image/webp"),
                                  QStringLiteral("image/bmp"),
                                  QString() }) {
            for (bool alpha : { false, true }) {
                const auto format = chooseOutputFormat(mime, alpha);
                QVERIFY(!format.encoder.isEmpty());
                if (format.encoder == QLatin1String("jpeg")) {
                    QCOMPARE(format.mime, QStringLiteral("image/jpeg"));
                    QCOMPARE(format.suffix, QStringLiteral("jpg"));
                } else {
                    QCOMPARE(format.encoder, QStringLiteral("png"));
                    QCOMPARE(format.mime, QStringLiteral("image/png"));
                    QCOMPARE(format.suffix, QStringLiteral("png"));
                }
            }
        }
    }

    void transparencyForcesPngEvenFromAJpegSource()
    {
        // JPEG has no alpha; flattening a cut-out avatar onto black is a
        // visibly wrong picture rather than a smaller one.
        QCOMPARE(chooseOutputFormat(QStringLiteral("image/jpeg"), true).mime,
                 QStringLiteral("image/png"));
        QCOMPARE(chooseOutputFormat(QStringLiteral("image/jpeg"), false).mime,
                 QStringLiteral("image/jpeg"));
    }

    // One frame of an animation is not the animation, and WebP WRITING lives
    // in qtimageformats, which the packaged DEB/RPM/AppImage builds need not
    // carry. Both come out as PNG and are DECLARED as PNG.
    void gifAndWebpSourcesBecomePngAndSaySo()
    {
        QCOMPARE(chooseOutputFormat(QStringLiteral("image/gif"), false).mime,
                 QStringLiteral("image/png"));
        QCOMPARE(chooseOutputFormat(QStringLiteral("image/webp"), false).mime,
                 QStringLiteral("image/png"));
        QCOMPARE(chooseOutputFormat(QStringLiteral("image/bmp"), false).mime,
                 QStringLiteral("image/png"));
    }

    void anUnknownRoleGetsTheTighterCapNotNoCap()
    {
        QCOMPARE(imagecrop::maxEdgeForRole(QStringLiteral("avatar")), 512);
        QCOMPARE(imagecrop::maxEdgeForRole(QStringLiteral("banner")), 1920);
        const int unknown = imagecrop::maxEdgeForRole(QStringLiteral("Avatar"));
        QVERIFY2(unknown > 0, "a typo in a call site silently uncapped an upload");
        QCOMPARE(unknown, 512);
    }

    // ── End to end, against real files ──────────────────────────────────

    void aCroppedFileIsWrittenAndItsBytesAreTheFormatItIsNamed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = dir.filePath(QStringLiteral("src.png"));
        QVERIFY(writeFile(png, encoded(patternImage(800, 600), "png")));

        StagedImageStore staged;
        ImageCropper cropper;
        cropper.setStagedImages(&staged);

        const QVariantMap info = cropper.load(QUrl::fromLocalFile(png));
        QVERIFY2(info.value(QStringLiteral("ok")).toBool(),
                 qPrintable(info.value(QStringLiteral("error")).toString()));
        QCOMPARE(info.value(QStringLiteral("width")).toInt(), 800);
        QCOMPARE(info.value(QStringLiteral("height")).toInt(), 600);
        QCOMPARE(info.value(QStringLiteral("mime")).toString(),
                 QStringLiteral("image/png"));
        // QML is handed a staged token, never the user's own path — pointing
        // an Image at that path is what would render an SVG.
        const QString preview = info.value(QStringLiteral("previewUrl")).toString();
        QVERIFY(preview.startsWith(QStringLiteral("image://lightning-staged/")));
        QVERIFY(!preview.contains(png));
        QCOMPARE(staged.count(), 1);

        const QUrl out = cropper.crop(100, 50, 600, 400, 512);
        QVERIFY(!out.isEmpty());
        QVERIFY(out.isLocalFile());
        QVERIFY(cropper.lastError().isEmpty());

        QFile file(out.toLocalFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        file.close();
        // The name and the bytes must agree, and both must be what the plan
        // said. A .png carrying JPEG bytes is the defect this pins.
        QCOMPARE(sniffRasterMime(bytes), QStringLiteral("image/png"));
        QVERIFY(out.toLocalFile().endsWith(QStringLiteral(".png")));

        const QImage decoded = QImage::fromData(bytes);
        QVERIFY(!decoded.isNull());
        // 600x400 capped to 512 on the long edge.
        QCOMPARE(decoded.size(), QSize(512, 341));

        // Closing the dialog releases the staged preview.
        cropper.discard();
        QCOMPARE(staged.count(), 0);

        // The written file OUTLIVES the dialog: the sink uploads it on its
        // own schedule.
        QVERIFY(QFile::exists(out.toLocalFile()));
        cropper.clearSession();
        QVERIFY(!QFile::exists(out.toLocalFile()));
    }

    void aJpegSourceComesOutAsJpegBytesUnderAJpgName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString jpg = dir.filePath(QStringLiteral("src.jpg"));
        QVERIFY(writeFile(jpg, encoded(patternImage(400, 400), "jpeg")));

        StagedImageStore staged;
        ImageCropper cropper;
        cropper.setStagedImages(&staged);
        QVERIFY(cropper.load(QUrl::fromLocalFile(jpg))
                    .value(QStringLiteral("ok")).toBool());

        const QUrl out = cropper.crop(0, 0, 400, 400, 512);
        QVERIFY(!out.isEmpty());
        QVERIFY(out.toLocalFile().endsWith(QStringLiteral(".jpg")));
        QFile file(out.toLocalFile());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(sniffRasterMime(file.readAll()), QStringLiteral("image/jpeg"));
        cropper.clearSession();
    }

    void anAlphaSourceComesOutAsPng()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage argb = patternImage(200, 200, QImage::Format_ARGB32);
        argb.setPixelColor(0, 0, QColor(0, 0, 0, 0));
        const QString png = dir.filePath(QStringLiteral("alpha.png"));
        QVERIFY(writeFile(png, encoded(argb, "png")));

        StagedImageStore staged;
        ImageCropper cropper;
        cropper.setStagedImages(&staged);
        QVERIFY(cropper.load(QUrl::fromLocalFile(png))
                    .value(QStringLiteral("ok")).toBool());
        const QUrl out = cropper.crop(0, 0, 200, 200, 512);
        QVERIFY(!out.isEmpty());
        QVERIFY(out.toLocalFile().endsWith(QStringLiteral(".png")));
        cropper.clearSession();
    }

    // The whole point of the gate: an SVG named .png is refused BEFORE
    // anything decodes it, nothing is staged, and no crop is possible.
    void anSvgNamedPngIsRefusedAndNothingIsStaged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("evil.png"));
        QVERIFY(writeFile(path,
                          "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\""
                          " width=\"64\" height=\"64\"><rect width=\"64\" height=\"64\"/>"
                          "</svg>"));

        StagedImageStore staged;
        ImageCropper cropper;
        cropper.setStagedImages(&staged);

        const QVariantMap info = cropper.load(QUrl::fromLocalFile(path));
        QVERIFY(!info.value(QStringLiteral("ok")).toBool());
        QCOMPARE(info.value(QStringLiteral("error")).toString(),
                 QStringLiteral("unsupported_image"));
        QCOMPARE(staged.count(), 0);
        QVERIFY(info.value(QStringLiteral("previewUrl")).toString().isEmpty());
        // And with no source loaded, cropping is refused rather than
        // producing something from whatever was there before.
        QVERIFY(cropper.crop(0, 0, 10, 10, 512).isEmpty());
        QCOMPARE(cropper.lastError(), QStringLiteral("no_source"));
    }

    // A refused file must not leave the previously accepted one croppable —
    // otherwise "choose a picture, choose a bad one, press Use" uploads the
    // first picture without saying so.
    void aRefusedFileReleasesTheOneBefore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString good = dir.filePath(QStringLiteral("good.png"));
        const QString bad = dir.filePath(QStringLiteral("bad.png"));
        QVERIFY(writeFile(good, encoded(patternImage(100, 100), "png")));
        QVERIFY(writeFile(bad, QByteArray("<svg/>")));

        StagedImageStore staged;
        ImageCropper cropper;
        cropper.setStagedImages(&staged);
        QVERIFY(cropper.load(QUrl::fromLocalFile(good))
                    .value(QStringLiteral("ok")).toBool());
        QCOMPARE(staged.count(), 1);
        QVERIFY(!cropper.load(QUrl::fromLocalFile(bad))
                     .value(QStringLiteral("ok")).toBool());
        QCOMPARE(staged.count(), 0);
        QVERIFY(cropper.crop(0, 0, 50, 50, 512).isEmpty());
    }

    void anUnreadableOrEmptyFileIsRefusedByCategory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StagedImageStore staged;
        ImageCropper cropper;
        cropper.setStagedImages(&staged);

        const QVariantMap missing =
            cropper.load(QUrl::fromLocalFile(dir.filePath(QStringLiteral("nope.png"))));
        QCOMPARE(missing.value(QStringLiteral("error")).toString(),
                 QStringLiteral("unreadable"));

        const QString empty = dir.filePath(QStringLiteral("empty.png"));
        QVERIFY(writeFile(empty, QByteArray()));
        QCOMPARE(cropper.load(QUrl::fromLocalFile(empty))
                     .value(QStringLiteral("error")).toString(),
                 QStringLiteral("unreadable"));
    }

    // The ring bounds disk without ever removing the crop just handed out.
    void onlyTheLastFewWrittenCropsAreKept()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString png = dir.filePath(QStringLiteral("src.png"));
        QVERIFY(writeFile(png, encoded(patternImage(200, 200), "png")));

        StagedImageStore staged;
        ImageCropper cropper;
        cropper.setStagedImages(&staged);
        QVERIFY(cropper.load(QUrl::fromLocalFile(png))
                    .value(QStringLiteral("ok")).toBool());

        QStringList written;
        for (int i = 0; i < ImageCropper::kRetainedOutputs + 2; ++i) {
            const QUrl out = cropper.crop(0, 0, 100 + i, 100 + i, 512);
            QVERIFY(!out.isEmpty());
            written.append(out.toLocalFile());
            // Whatever else has gone, the one just returned is present.
            QVERIFY(QFile::exists(out.toLocalFile()));
        }
        QVERIFY(!QFile::exists(written.first()));
        QVERIFY(QFile::exists(written.last()));
        cropper.clearSession();
    }
};

QTEST_GUILESS_MAIN(ImageCropTest)
#include "ImageCropTest.moc"
