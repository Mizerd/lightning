// StormBandPainter: the exact port of storm-band-export.html's painters.
// Pins the reference-fixed geometry of every layer, byte-for-byte
// determinism (same inputs -> identical images), theme sensitivity, the
// palette's light/dark branch, and defensive id parsing.
#include "app/StormBandPainter.h"

#include <QtTest/QtTest>

class StormBandPainterTest : public QObject
{
    Q_OBJECT

private:
    static QString storm(const char *layer)
    {
        return QStringLiteral("%1?bg=080C1C&accent=FFD447")
            .arg(QLatin1String(layer));
    }

private Q_SLOTS:
    // Every layer answers at its reference-fixed size — the tile widths and
    // sprite boxes the HTML hard-codes (source art, rendered 2x by QML).
    void everyLayerHasItsReferenceGeometry()
    {
        const struct { const char *layer; int w; int h; } expected[] = {
            { "gradient", 8, 190 }, { "sky", 768, 66 },   { "mist", 545, 66 },
            { "far", 331, 66 },     { "mid", 397, 66 },   { "spire", 641, 66 },
            { "front", 719, 66 },   { "rain", 160, 66 },  { "moon", 18, 18 },
            { "shoot", 30, 12 },    { "horizon", 8, 70 }, { "bloomA", 340, 240 },
            { "bloomB", 340, 240 }, { "bloomC", 340, 240 },
            { "ember1", 2, 2 },     { "ember2", 2, 2 },   { "ember3", 2, 2 },
            // 46x74 art plus the 8px baked-glow pad ring on each side.
            { "bolt1", 62, 90 },    { "bolt6", 62, 90 },
        };
        for (const auto &e : expected) {
            const QImage img = stormband::imageForId(storm(e.layer));
            QVERIFY2(!img.isNull(), e.layer);
            QCOMPARE(img.width(), e.w);
            QCOMPARE(img.height(), e.h);
        }
    }

    // The PRNG port must match the reference exactly; determinism is the
    // whole point of the seeded painters.
    void prngIsDeterministicAndWellDistributed()
    {
        stormband::Rnd a(11), b(11);
        for (int i = 0; i < 1000; ++i) {
            const double v = a();
            QCOMPARE(v, b());
            QVERIFY(v >= 0.0 && v < 1.0);
        }
        // Different seeds diverge immediately.
        stormband::Rnd c(11), d(12);
        bool differed = false;
        for (int i = 0; i < 8 && !differed; ++i)
            differed = c() != d();
        QVERIFY(differed);
    }

    void sameInputsProduceIdenticalImages()
    {
        for (const char *layer :
             { "sky", "spire", "front", "rain", "bolt3", "mist" }) {
            const QImage a = stormband::imageForId(storm(layer));
            const QImage b = stormband::imageForId(storm(layer));
            QVERIFY2(a == b, layer);
        }
    }

    void darkAndLightThemesProduceDifferentScenes()
    {
        const QImage dark = stormband::imageForId(
            QStringLiteral("sky?bg=080C1C&accent=FFD447"));
        const QImage light = stormband::imageForId(
            QStringLiteral("sky?bg=F7F7F5&accent=12A67F"));
        QVERIFY(!dark.isNull() && !light.isNull());
        QVERIFY(dark != light);
        // The reference's luminance branch: dark bg -> stars, light -> dust.
        QVERIFY(!stormband::derivePalette(QColor("#080C1C"),
                                          QColor("#FFD447")).light);
        QVERIFY(stormband::derivePalette(QColor("#F7F7F5"),
                                         QColor("#12A67F")).light);
    }

    void acceptsOnlyWellFormedIds()
    {
        const QStringList junk = {
            QStringLiteral(""),
            QStringLiteral("sky"),                       // no colors
            QStringLiteral("sky?bg=080C1C"),             // missing accent
            QStringLiteral("sky?bg=xyzxyz&accent=FFD447"),
            QStringLiteral("sky?bg=080C1C&accent=FFD4477"), // 7 hex digits
            QStringLiteral("nolayer?bg=080C1C&accent=FFD447"),
            QStringLiteral("bolt9?bg=080C1C&accent=FFD447"),
            QStringLiteral("../../etc?bg=080C1C&accent=FFD447"),
        };
        for (const QString &id : junk)
            QVERIFY2(stormband::imageForId(id).isNull(), qPrintable(id));
    }

    void providerReportsSizeAndIgnoresRequestedSize()
    {
        StormBandImageProvider provider;
        QSize size;
        const QImage img =
            provider.requestImage(storm("sky"), &size, QSize(10, 10));
        QCOMPARE(img.size(), QSize(768, 66));
        QCOMPARE(size, QSize(768, 66));
        QVERIFY(provider.requestImage(QStringLiteral("bad"), &size, {})
                    .isNull());
    }

    // The scene actually derives from the accent: two accents on the same
    // background disagree on accent-derived layers (bolts, embers, blooms).
    void accentReachesTheAccentDerivedLayers()
    {
        for (const char *layer : { "bolt1", "ember1", "bloomA", "horizon" }) {
            const QImage a = stormband::imageForId(
                QStringLiteral("%1?bg=080C1C&accent=FFD447")
                    .arg(QLatin1String(layer)));
            const QImage b = stormband::imageForId(
                QStringLiteral("%1?bg=080C1C&accent=27C2AD")
                    .arg(QLatin1String(layer)));
            QVERIFY2(!a.isNull() && !b.isNull() && a != b, layer);
        }
    }
};

QTEST_MAIN(StormBandPainterTest)
#include "StormBandPainterTest.moc"
