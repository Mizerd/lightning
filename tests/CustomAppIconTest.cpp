#include "app/CustomAppIcon.h"
#include "app/SettingsManager.h"
#include "media/ImageFormatSupport.h"
#include "storage/AppDataPaths.h"

#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

// The custom-application-icon import pipeline: untrusted bytes -> sniffed
// raster whitelist -> bounded decode -> circular 512px normalization, plus
// the persisted enable flag and the app-data path shape. The old tree had
// no custom icon support at all, so this suite is the feature's regression
// net.
class CustomAppIconTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;

    static QByteArray encoded(const QImage &image, const char *format)
    {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, format);
        return bytes;
    }

    static QImage solid(int w, int h, QColor color)
    {
        QImage image(w, h, QImage::Format_ARGB32);
        image.fill(color);
        return image;
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_config = std::make_unique<QTemporaryDir>();
        QVERIFY(m_config->isValid());
        qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
        qputenv("XDG_DATA_HOME", m_config->path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("LightningIconTest"));
        QCoreApplication::setApplicationName(QStringLiteral("custom-app-icon"));
    }

    void sniffAcceptsExactlyTheRasterWhitelist()
    {
        QCOMPARE(appicon::sniffedRasterFormat(
                     encoded(solid(24, 24, Qt::red), "PNG")),
                 QStringLiteral("png"));
        QCOMPARE(appicon::sniffedRasterFormat(
                     encoded(solid(24, 24, Qt::red), "JPEG")),
                 QStringLiteral("jpeg"));
        QCOMPARE(appicon::sniffedRasterFormat(
                     encoded(solid(24, 24, Qt::red), "BMP")),
                 QStringLiteral("bmp"));
        QCOMPARE(appicon::sniffedRasterFormat(
                     QByteArray("GIF89a") + QByteArray(20, '\0')),
                 QStringLiteral("gif"));
        QCOMPARE(appicon::sniffedRasterFormat(
                     QByteArray("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)),
                 QStringLiteral("webp"));
        QVERIFY(appicon::sniffedRasterFormat(
                    QByteArray("<svg xmlns='x'></svg>")).isEmpty());
        QVERIFY(appicon::sniffedRasterFormat(
                    QByteArray("  <?xml version='1.0'?><svg/>")).isEmpty());
        QVERIFY(appicon::sniffedRasterFormat(QByteArray("hello world!")).isEmpty());
        QVERIFY(appicon::sniffedRasterFormat(QByteArray()).isEmpty());
    }

    void pngNormalizesToCircular512()
    {
        const auto result =
            appicon::normalizeIconBytes(encoded(solid(100, 100, Qt::red), "PNG"));
        QVERIFY(result.ok);
        QCOMPARE(result.category, QString());
        QCOMPARE(result.image.width(), appicon::kNormalizedEdge);
        QCOMPARE(result.image.height(), appicon::kNormalizedEdge);
        // Corners transparent (outside the circle), center opaque red.
        QCOMPARE(qAlpha(result.image.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(result.image.pixel(511, 511)), 0);
        QVERIFY(qAlpha(result.image.pixel(256, 256)) == 255);
        QCOMPARE(qRed(result.image.pixel(256, 256)), 255);
    }

    void jpegWithoutAlphaStillGetsTransparentCorners()
    {
        const auto result =
            appicon::normalizeIconBytes(encoded(solid(64, 64, Qt::blue), "JPEG"));
        QVERIFY(result.ok);
        QCOMPARE(qAlpha(result.image.pixel(0, 0)), 0);
        QVERIFY(qAlpha(result.image.pixel(256, 256)) == 255);
    }

    void nonSquareInputIsCenterCroppedNotStretched()
    {
        // Left half green, right half blue; the 200x100 input center-crops
        // to x=[50,150) so both halves survive into the square.
        QImage wide(200, 100, QImage::Format_ARGB32);
        wide.fill(Qt::green);
        for (int y = 0; y < 100; ++y)
            for (int x = 100; x < 200; ++x)
                wide.setPixel(x, y, qRgb(0, 0, 255));
        const auto result = appicon::normalizeIconBytes(encoded(wide, "PNG"));
        QVERIFY(result.ok);
        QCOMPARE(qBlue(result.image.pixel(150, 256)), 0);   // left = green
        QCOMPARE(qGreen(result.image.pixel(150, 256)), 255);
        QCOMPARE(qBlue(result.image.pixel(360, 256)), 255); // right = blue
    }

    void rejectsSvgAndMarkup()
    {
        QCOMPARE(appicon::normalizeIconBytes(
                     QByteArray("<svg xmlns='http://www.w3.org/2000/svg'/>"))
                     .category,
                 QStringLiteral("svg_rejected"));
        QCOMPARE(appicon::normalizeIconBytes(
                     QByteArray("<?xml version='1.0'?><svg/>"))
                     .category,
                 QStringLiteral("svg_rejected"));
    }

    void rejectsNonRasterBytes()
    {
        QCOMPARE(appicon::normalizeIconBytes(QByteArray("plain text file"))
                     .category,
                 QStringLiteral("unsupported_format"));
        QCOMPARE(appicon::normalizeIconBytes(QByteArray()).category,
                 QStringLiteral("empty"));
    }

    void rejectsSpoofedMagicWithGarbageBody()
    {
        // Real PNG signature followed by junk must fail the decode, never
        // produce an icon.
        QByteArray fake("\x89PNG\r\n\x1a\n");
        fake += QByteArray(64, 'x');
        QCOMPARE(appicon::normalizeIconBytes(fake).category,
                 QStringLiteral("decode_failed"));
    }

    void rejectsOutOfBoundsDimensions()
    {
        QCOMPARE(appicon::normalizeIconBytes(
                     encoded(solid(8, 8, Qt::red), "PNG"))
                     .category,
                 QStringLiteral("too_small"));
        QCOMPARE(appicon::normalizeIconBytes(
                     encoded(solid(9000, 20, Qt::red), "PNG"))
                     .category,
                 QStringLiteral("too_large_dimensions"));
    }

    void rejectsOversizedBytes()
    {
        QByteArray huge(appicon::kMaxInputBytes + 1, 'x');
        QCOMPARE(appicon::normalizeIconBytes(huge).category,
                 QStringLiteral("too_large_bytes"));
    }

    void settingPersistsAcrossManagers()
    {
        SettingsManager settings;
        QCOMPARE(settings.customAppIconEnabled(), false);
        QSignalSpy spy(&settings, &SettingsManager::customAppIconEnabledChanged);
        settings.setCustomAppIconEnabled(true);
        QCOMPARE(spy.count(), 1);
        settings.setCustomAppIconEnabled(true); // no duplicate signal
        QCOMPARE(spy.count(), 1);
        SettingsManager reloaded;
        QCOMPARE(reloaded.customAppIconEnabled(), true);
        reloaded.setCustomAppIconEnabled(false);
        SettingsManager again;
        QCOMPARE(again.customAppIconEnabled(), false);
    }

    void appDataPathIsDeviceGlobalAndNormalized()
    {
        const QString path = matrix::app_data::customAppIconFile();
        QVERIFY(path.endsWith(QStringLiteral("/branding/custom-app-icon.png")));
        // Device-global: no account slug segment between root and branding.
        QVERIFY(!path.contains(QStringLiteral("starred-gifs")));
    }

    // --- shared image-format table (src/media/ImageFormatSupport.h) ---------
    //
    // These cover the 2026-08-28 round: every Linux package shipped exactly
    // three Qt image plugins (gif/ico/jpeg) while the client's own sniffers
    // ACCEPTED image/webp, so it accepted a format it could not draw. The
    // sniffer and the decoder must not silently disagree again.

    void sniffIdentifiesBothJpegXlShapes()
    {
        // REAL cjxl (libjxl 0.11) output for a 32x32 image, not a hand-built
        // signature: a bare codestream and the ISOBMFF container form.
        const QByteArray codestream(
            "\xff\x0a\x47\x06\x00\x13\x88\x02\x00\xf0\x00\xb5\x9f\x20\x00\x00"
            "\x15\x2a\xa3\x8c\x1b\xbc\x9c\xeb\xf9\xf2\x43\x87\xc5\xb4\x8d\xeb"
            "\x0c\x6d\xb5\x6d\x61\x09\x63\xb3\xbd\x30\x48\x48\x38\x43\xbc\xcd"
            "\x51\x45\xf3\x2c\x05\x00\x70\x64\x8a\x02\xc0\x4e\xd0\x04\x00\x30"
            "\x8c\xc6\x00\x00\x95\x24\x00", 71);
        const QByteArray container(
            "\x00\x00\x00\x0c\x4a\x58\x4c\x20\x0d\x0a\x87\x0a\x00\x00\x00\x14"
            "\x66\x74\x79\x70\x6a\x78\x6c\x20\x00\x00\x00\x00\x6a\x78\x6c\x20"
            "\x00\x00\x00\x4f\x6a\x78\x6c\x63\xff\x0a\x47\x06\x00\x13\x88\x02"
            "\x00\xf0\x00\xb5\x9f\x20\x00\x00\x15\x2a\xa3\x8c\x1b\xbc\x9c\xeb"
            "\xf9\xf2\x43\x87\xc5\xb4\x8d\xeb\x0c\x6d\xb5\x6d\x61\x09\x63\xb3"
            "\xbd\x30\x48\x48\x38\x43\xbc\xcd\x51\x45\xf3\x2c\x05\x00\x70\x64"
            "\x8a\x02\xc0\x4e\xd0\x04\x00\x30\x8c\xc6\x00\x00\x95\x24\x00", 111);

        QCOMPARE(lightning::imagefmt::sniffRasterMime(codestream),
                 QStringLiteral("image/jxl"));
        QCOMPARE(lightning::imagefmt::sniffRasterMime(container),
                 QStringLiteral("image/jxl"));
        // And the icon path sees the same thing through its own entry point.
        QCOMPARE(appicon::sniffedRasterFormat(codestream),
                 QStringLiteral("jxl"));

        // A JPEG shares only its first byte with a JXL codestream and must
        // never be swallowed by the two-byte signature.
        QCOMPARE(lightning::imagefmt::sniffRasterMime(
                     encoded(solid(24, 24, Qt::red), "JPEG")),
                 QStringLiteral("image/jpeg"));
    }

    void iconSnifferDelegatesToTheSharedTable()
    {
        // The five signatures used to be copy-pasted into CustomAppIcon,
        // ForwardController, ImageCropper, AppController and the Rust bridge,
        // with different format lists. Pin that the icon path and the shared
        // table now agree, so a future addition to one reaches the other.
        const QList<QByteArray> samples = {
            encoded(solid(24, 24, Qt::red), "PNG"),
            encoded(solid(24, 24, Qt::red), "JPEG"),
            encoded(solid(24, 24, Qt::red), "BMP"),
            QByteArray("GIF89a") + QByteArray(20, '\0'),
            QByteArray("RIFF\x10\x00\x00\x00WEBPVP8 ", 16),
        };
        for (const QByteArray &s : samples) {
            QCOMPARE(appicon::sniffedRasterFormat(s),
                     lightning::imagefmt::sniffRasterQtFormat(s));
            QVERIFY(!appicon::sniffedRasterFormat(s).isEmpty());
        }
    }

    void decodabilityIsAskedOfTheDecoderNeverAssumed()
    {
        using namespace lightning::imagefmt;
        // EXACTLY what every Linux package shipped up to 0.8.0: qtbase's own
        // plugins and nothing else. WebP is required and missing, which is the
        // accept/decode disagreement; JPEG XL is optional and missing, which
        // is a truthful platform limit.
        const QSet<QString> packaged0_8_0 = {
            QStringLiteral("png"),  QStringLiteral("jpeg"),
            QStringLiteral("gif"),  QStringLiteral("bmp"),
            QStringLiteral("ico"),  QStringLiteral("ppm"),
        };
        QCOMPARE(undecodableWith(packaged0_8_0, /*requiredOnly=*/true),
                 QStringList{ QStringLiteral("image/webp") });
        QVERIFY(undecodableWith(packaged0_8_0, /*requiredOnly=*/false)
                    .contains(QStringLiteral("image/jxl")));

        // A Linux package after this round: webp and jxl both present.
        QSet<QString> fixed = packaged0_8_0;
        fixed.insert(QStringLiteral("webp"));
        fixed.insert(QStringLiteral("jxl"));
        QVERIFY(undecodableWith(fixed, false).isEmpty());

        // Windows/macOS after this round: webp present, jxl genuinely absent.
        QSet<QString> noJxl = packaged0_8_0;
        noJxl.insert(QStringLiteral("webp"));
        QVERIFY(undecodableWith(noJxl, /*requiredOnly=*/true).isEmpty());
        QCOMPARE(undecodableWith(noJxl, /*requiredOnly=*/false),
                 QStringList{ QStringLiteral("image/jxl") });

        QVERIFY(!canDecodeWith(packaged0_8_0, QStringLiteral("webp")));
        QVERIFY(canDecodeWith(packaged0_8_0, QStringLiteral("png")));
        // An empty format name is never decodable — the "no plugin" answer
        // and the "not identified" answer must not collapse into each other.
        QVERIFY(!canDecodeWith(fixed, QString()));
    }

    void anUndecodableFormatIsNotReportedAsCorruption()
    {
        // A real, well-formed 32x32 JPEG XL. Whichever way this build is
        // packaged, exactly one of two answers is honest: it decodes, or it
        // says the BUILD has no plugin. It must never claim the bytes are
        // unsupported (they are a format Lightning identifies) and must never
        // claim the decode failed (the file is fine).
        const QByteArray jxl(
            "\xff\x0a\x47\x06\x00\x13\x88\x02\x00\xf0\x00\xb5\x9f\x20\x00\x00"
            "\x15\x2a\xa3\x8c\x1b\xbc\x9c\xeb\xf9\xf2\x43\x87\xc5\xb4\x8d\xeb"
            "\x0c\x6d\xb5\x6d\x61\x09\x63\xb3\xbd\x30\x48\x48\x38\x43\xbc\xcd"
            "\x51\x45\xf3\x2c\x05\x00\x70\x64\x8a\x02\xc0\x4e\xd0\x04\x00\x30"
            "\x8c\xc6\x00\x00\x95\x24\x00", 71);
        const appicon::NormalizeResult r = appicon::normalizeIconBytes(jxl);
        if (r.ok) {
            QCOMPARE(r.image.width(), appicon::kNormalizedEdge);
        } else {
            QCOMPARE(r.category, QStringLiteral("format_not_decodable"));
        }
        QVERIFY(r.category != QStringLiteral("unsupported_format"));
        QVERIFY(r.category != QStringLiteral("decode_failed"));
    }
};

QTEST_MAIN(CustomAppIconTest)
#include "CustomAppIconTest.moc"
