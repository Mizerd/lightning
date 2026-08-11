#include "app/CustomAppIcon.h"
#include "app/SettingsManager.h"
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
};

QTEST_MAIN(CustomAppIconTest)
#include "CustomAppIconTest.moc"
