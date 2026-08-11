// Storm Band pixel-art tile generation. Pins the pure id-parsing and
// rendering contract: every recognised layer returns a non-null image at
// the exact requested size; identical inputs are byte-identical (the QML
// scroll/animation contract depends on this — a tile must never redraw
// differently between two adjacent copies); dark vs light inputs diverge
// visibly; and malformed/unknown ids are refused safely rather than
// crashing or silently substituting a placeholder. No GUI/windowing needed
// — QImage/QPainter software rendering runs fine under QTEST_GUILESS_MAIN,
// mirroring tests/QrImageProviderTest.cpp.

#include "app/StormBandPainter.h"

#include <QImage>
#include <QtTest>

#include <cstring>

namespace {

QString idFor(const QString &layer, int w, int h, bool dark,
             const QString &base, const QString &accent, quint32 seed = 7)
{
    return QStringLiteral("%1/%2x%3?dark=%4&base=%5&accent=%6&seed=%7")
        .arg(layer)
        .arg(w)
        .arg(h)
        .arg(dark ? 1 : 0)
        .arg(base)
        .arg(accent)
        .arg(seed);
}

} // namespace

class StormBandPainterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Every recognised layer renders a non-null image at exactly the
    // requested logical size — never a placeholder, never a size mismatch
    // that would desync the QML tiling/scroll maths.
    void everyLayerRendersNonNullAtRequestedSize()
    {
        const QStringList layers = {
            QStringLiteral("sky"),      QStringLiteral("farhills"),
            QStringLiteral("mist"),     QStringLiteral("midhills"),
            QStringLiteral("spires"),   QStringLiteral("ridge"),
            QStringLiteral("rain"),     QStringLiteral("bolt"),
            QStringLiteral("bloom"),    QStringLiteral("streak"),
        };
        for (const QString &layer : layers) {
            stormband::SceneParams params;
            QVERIFY2(stormband::parseId(
                         idFor(layer, 200, 96, true, QStringLiteral("0A0F24"),
                              QStringLiteral("FFD447")),
                         &params),
                     qPrintable(layer));
            const QImage img = stormband::renderLayer(params);
            QVERIFY2(!img.isNull(), qPrintable(layer));
            QCOMPARE(img.width(), 200);
            QCOMPARE(img.height(), 96);
        }
    }

    // Determinism: the same id, rendered twice, must be byte-identical —
    // never QRandomGenerator::global(), never a time source.
    void identicalInputsProduceByteIdenticalImages()
    {
        const QString id = idFor(QStringLiteral("ridge"), 360, 84, true,
                                 QStringLiteral("0D1B45"),
                                 QStringLiteral("FFD447"), 4242);
        stormband::SceneParams a;
        stormband::SceneParams b;
        QVERIFY(stormband::parseId(id, &a));
        QVERIFY(stormband::parseId(id, &b));

        const QImage imgA =
            stormband::renderLayer(a).convertToFormat(QImage::Format_ARGB32);
        const QImage imgB =
            stormband::renderLayer(b).convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!imgA.isNull());
        QCOMPARE(imgA.sizeInBytes(), imgB.sizeInBytes());
        QVERIFY(std::memcmp(imgA.constBits(), imgB.constBits(),
                            size_t(imgA.sizeInBytes())) == 0);
    }

    // A different seed must be able to move at least one pixel — otherwise
    // `seed` would be a decoration, not an actual input.
    void differentSeedsProduceDifferentPixels()
    {
        stormband::SceneParams a;
        stormband::SceneParams b;
        QVERIFY(stormband::parseId(
            idFor(QStringLiteral("ridge"), 360, 84, true,
                 QStringLiteral("0D1B45"), QStringLiteral("FFD447"), 1),
            &a));
        QVERIFY(stormband::parseId(
            idFor(QStringLiteral("ridge"), 360, 84, true,
                 QStringLiteral("0D1B45"), QStringLiteral("FFD447"), 2),
            &b));
        const QImage imgA = stormband::renderLayer(a);
        const QImage imgB = stormband::renderLayer(b);
        QVERIFY(!imgA.isNull() && !imgB.isNull());
        QVERIFY(imgA != imgB);
    }

    // dark=1 and dark=0 (with the corresponding light/dark base+accent a
    // real theme pair would carry) must diverge visibly.
    void darkAndLightInputsProduceDifferentPixels()
    {
        stormband::SceneParams dark;
        stormband::SceneParams light;
        QVERIFY(stormband::parseId(
            idFor(QStringLiteral("sky"), 300, 96, true,
                 QStringLiteral("0A0F24"), QStringLiteral("FFD447")),
            &dark));
        QVERIFY(stormband::parseId(
            idFor(QStringLiteral("sky"), 300, 96, false,
                 QStringLiteral("F7F7F5"), QStringLiteral("12A67F")),
            &light));
        const QImage imgDark = stormband::renderLayer(dark);
        const QImage imgLight = stormband::renderLayer(light);
        QVERIFY(!imgDark.isNull() && !imgLight.isNull());
        QVERIFY(imgDark != imgLight);
    }

    // Malformed/unknown ids are refused defensively — no crash, no
    // substitute image, `parseId` simply returns false.
    void malformedIdsAreRejectedSafely()
    {
        const QStringList junk = {
            QString(),
            QStringLiteral("sky"),
            QStringLiteral("sky/"),
            QStringLiteral("nonsense/200x96?dark=0&base=FFFFFF&accent=000000"),
            QStringLiteral("sky/0x96?dark=0&base=FFFFFF&accent=000000"),
            QStringLiteral("sky/200x0?dark=0&base=FFFFFF&accent=000000"),
            QStringLiteral("sky/200x96?dark=7&base=FFFFFF&accent=000000"),
            QStringLiteral("sky/200x96?dark=0&base=NOTAHEX&accent=000000"),
            QStringLiteral("sky/200x96?dark=0&base=FFFFFF"),
            QStringLiteral("sky/200x96"),
            QStringLiteral("../etc/passwd/1x1?dark=0&base=FFFFFF&accent=000000"),
            QStringLiteral("sky/99999x99999?dark=0&base=FFFFFF&accent=000000"),
            QStringLiteral("sky/-4x96?dark=0&base=FFFFFF&accent=000000"),
        };
        for (const QString &id : junk) {
            stormband::SceneParams params;
            QVERIFY2(!stormband::parseId(id, &params), qPrintable(id));
        }

        // Even a directly-constructed invalid SceneParams must be refused
        // by renderLayer() rather than crash or return garbage.
        stormband::SceneParams unknown;
        unknown.layer = stormband::Layer::Unknown;
        unknown.size = QSize(100, 100);
        QVERIFY(stormband::renderLayer(unknown).isNull());

        stormband::SceneParams zeroSize;
        zeroSize.layer = stormband::Layer::Sky;
        zeroSize.size = QSize(0, 0);
        QVERIFY(stormband::renderLayer(zeroSize).isNull());
    }

    // The QML-facing provider: unknown ids serve nothing; known ids serve
    // exactly the requested size (and report it through `size`, matching
    // main.cpp's other providers — see QrImageProvider).
    void providerServesNullForUnknownAndValidForKnownIds()
    {
        StormBandImageProvider provider;
        QSize size;
        const QImage missing = provider.requestImage(
            QStringLiteral("nope/10x10?dark=0&base=FFFFFF&accent=000000"),
            &size, QSize());
        QVERIFY(missing.isNull());

        const QImage present = provider.requestImage(
            idFor(QStringLiteral("bolt"), 34, 130, true,
                 QStringLiteral("0A0F24"), QStringLiteral("FFD447")),
            &size, QSize());
        QVERIFY(!present.isNull());
        QCOMPARE(present.width(), 34);
        QCOMPARE(present.height(), 130);
        QCOMPARE(size, present.size());
    }
};

QTEST_GUILESS_MAIN(StormBandPainterTest)
#include "StormBandPainterTest.moc"
