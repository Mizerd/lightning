// Show-QR verification rendering. Lightning DISPLAYS a verification QR code
// for another device to scan; it never scans one. This pins the pure
// grid -> QImage transform and the memory-only single-slot store:
//   * the packed row-major/MSB-first grid maps to the right modules;
//   * the 4-module quiet zone is present and white;
//   * dark modules are black and light ones white regardless of theme;
//   * scaling stays whole-module (a fractional module will not scan);
//   * an unknown or stale token renders NOTHING rather than another flow's
//     code;
//   * malformed geometry is refused rather than rendered sheared;
//   * clear() drops the grid, so a code cannot outlive its flow.
// No credentials, key material, or real QR payloads appear anywhere here —
// the grids are synthetic.

#include "crypto/QrImageProvider.h"

#include <QImage>
#include <QtTest>

namespace {

// Pack a square bool grid exactly as the Rust bridge does: row-major, MSB
// first within each byte, every row starting on a fresh byte.
QByteArray pack(const QVector<bool> &modules, int size)
{
    const int stride = (size + 7) / 8;
    QByteArray out(stride * size, '\0');
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (modules[y * size + x])
                out[y * stride + x / 8] = static_cast<char>(
                    static_cast<uchar>(out[y * stride + x / 8])
                    | (0x80u >> (x % 8)));
        }
    }
    return out;
}

constexpr int kQuiet = QrImageProvider::kQuietZoneModules;

} // namespace

class QrImageProviderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A 3x3 grid with the corners set proves orientation as well as
    // packing: a transposed or bit-reversed unpack would place the marks
    // somewhere else.
    void gridModulesLandWhereTheGridSaysAndKeepAQuietZone()
    {
        constexpr int size = 3;
        QVector<bool> modules(size * size, false);
        modules[0] = true;                    // (0,0)
        modules[2] = true;                    // (2,0)
        modules[size * 2] = true;             // (0,2)
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("tok"), size,
                              pack(modules, size)));

        QrImageProvider provider(&store);
        QSize outSize;
        // Ask for exactly one pixel per module so image coordinates are
        // module coordinates.
        const int total = size + 2 * kQuiet;
        const QImage img = provider.requestImage(QStringLiteral("tok"),
                                                 &outSize, QSize(total, total));
        QCOMPARE(img.size(), QSize(total, total));
        QCOMPARE(outSize, QSize(total, total));

        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 0), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 2, kQuiet + 0), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 2), QColor(Qt::black));
        // Clear modules stay light.
        QCOMPARE(img.pixelColor(kQuiet + 1, kQuiet + 1), QColor(Qt::white));
        QCOMPARE(img.pixelColor(kQuiet + 2, kQuiet + 2), QColor(Qt::white));

        // The quiet zone is required by the QR specification: without it a
        // camera cannot find the code's edges.
        for (int i = 0; i < total; ++i) {
            QCOMPARE(img.pixelColor(i, 0), QColor(Qt::white));
            QCOMPARE(img.pixelColor(0, i), QColor(Qt::white));
            QCOMPARE(img.pixelColor(i, total - 1), QColor(Qt::white));
            QCOMPARE(img.pixelColor(total - 1, i), QColor(Qt::white));
        }
    }

    // A row wider than one byte must not bleed into the next row.
    void rowsWiderThanAByteDoNotShear()
    {
        constexpr int size = 9;
        QVector<bool> modules(size * size, false);
        modules[8] = true;                    // (8,0) — last bit of row 0
        modules[size * 1 + 0] = true;         // (0,1) — first bit of row 1
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("t"), size, pack(modules, size)));

        QrImageProvider provider(&store);
        const int total = size + 2 * kQuiet;
        const QImage img = provider.requestImage(QStringLiteral("t"), nullptr,
                                                 QSize(total, total));
        QCOMPARE(img.pixelColor(kQuiet + 8, kQuiet + 0), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 1), QColor(Qt::black));
        // The padding bits past module 8 must not paint anything.
        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 0), QColor(Qt::white));
        QCOMPARE(img.pixelColor(kQuiet + 8, kQuiet + 1), QColor(Qt::white));
    }

    // Whole-module scaling only. A fractional module size is precisely what
    // makes a rendered code fail to scan, so the provider rounds the
    // requested size DOWN to a whole multiple of the module count.
    void scalingStaysOnWholeModules()
    {
        constexpr int size = 5;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("s"), size,
                              pack(QVector<bool>(size * size, true), size)));
        QrImageProvider provider(&store);
        const int total = size + 2 * kQuiet;   // 13

        // 100 / 13 = 7 (truncated) -> 91, never 100.
        const QImage img = provider.requestImage(QStringLiteral("s"), nullptr,
                                                 QSize(100, 100));
        QCOMPARE(img.width() % total, 0);
        QCOMPARE(img.height(), img.width());
        QCOMPARE(img.width(), total * (100 / total));

        // Nearest-neighbour: an upscaled module is a solid block, never a
        // blurred gradient.
        QCOMPARE(img.pixelColor(kQuiet * (100 / total), kQuiet * (100 / total)),
                 QColor(Qt::black));
    }

    void withoutARequestedSizeItStillRendersWholeModules()
    {
        constexpr int size = 5;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("d"), size,
                              pack(QVector<bool>(size * size, false), size)));
        QrImageProvider provider(&store);
        const int total = size + 2 * kQuiet;

        const QImage img =
            provider.requestImage(QStringLiteral("d"), nullptr, QSize());
        QVERIFY(!img.isNull());
        QCOMPARE(img.width() % total, 0);
    }

    // A stale URL must render nothing. Serving whatever code happens to be
    // stored would show one flow's secret under another flow's URL.
    void anUnknownOrStaleTokenRendersNothing()
    {
        constexpr int size = 3;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("current"), size,
                              pack(QVector<bool>(size * size, true), size)));
        QrImageProvider provider(&store);

        QSize outSize(99, 99);
        QVERIFY(provider.requestImage(QStringLiteral("stale"), &outSize,
                                      QSize(64, 64))
                    .isNull());
        QCOMPARE(outSize, QSize());
        QVERIFY(provider.requestImage(QString{}, nullptr, QSize(64, 64))
                    .isNull());
        // The real token still works — rejection is token-scoped, not a
        // wholesale disable.
        QVERIFY(!provider.requestImage(QStringLiteral("current"), nullptr,
                                       QSize(64, 64))
                     .isNull());
    }

    // Malformed geometry is refused at the store, so nothing sheared or
    // truncated can ever reach a screen and be presented as scannable.
    void malformedGeometryIsRefusedByTheStore()
    {
        QrCodeStore store;
        // Wrong byte count for the module count.
        QVERIFY(!store.setCode(QStringLiteral("t"), 3, QByteArray(2, '\0')));
        // Zero and negative module counts.
        QVERIFY(!store.setCode(QStringLiteral("t"), 0, QByteArray()));
        QVERIFY(!store.setCode(QStringLiteral("t"), -1, QByteArray()));
        // Beyond the accepted bound (QR version 40 is 177 modules).
        const int tooBig = QrCodeStore::kMaxModules + 1;
        QVERIFY(!store.setCode(QStringLiteral("t"), tooBig,
                               QByteArray(((tooBig + 7) / 8) * tooBig, '\0')));
        // An empty token cannot key anything.
        QVERIFY(!store.setCode(QString{}, 3, QByteArray(3, '\0')));

        // Nothing was stored by any of those.
        int modules = -1;
        QVERIFY(store.gridFor(QStringLiteral("t"), &modules).isEmpty());
        QCOMPARE(modules, 0);
    }

    // A code must not outlive the flow it belongs to. AppController calls
    // clear() on every terminal state, on cancel, on reset and on logout.
    void clearingDropsTheCodeSoItCannotOutliveItsFlow()
    {
        constexpr int size = 3;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("live"), size,
                              pack(QVector<bool>(size * size, true), size)));
        QrImageProvider provider(&store);
        QVERIFY(!provider.requestImage(QStringLiteral("live"), nullptr,
                                       QSize(64, 64))
                     .isNull());

        store.clear();

        int modules = -1;
        QVERIFY(store.gridFor(QStringLiteral("live"), &modules).isEmpty());
        QCOMPARE(modules, 0);
        QVERIFY(provider.requestImage(QStringLiteral("live"), nullptr,
                                      QSize(64, 64))
                    .isNull());
    }

    // Storing a second code replaces the first: the bridge enforces one
    // flow at a time, and the old token must stop resolving immediately.
    void storingANewCodeRetiresThePreviousToken()
    {
        constexpr int size = 3;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("first"), size,
                              pack(QVector<bool>(size * size, true), size)));
        QVERIFY(store.setCode(QStringLiteral("second"), size,
                              pack(QVector<bool>(size * size, false), size)));

        QrImageProvider provider(&store);
        QVERIFY(provider.requestImage(QStringLiteral("first"), nullptr,
                                      QSize(64, 64))
                    .isNull());
        QVERIFY(!provider.requestImage(QStringLiteral("second"), nullptr,
                                       QSize(64, 64))
                     .isNull());
    }

    // A rejected setCode must not disturb the code already on screen.
    void arejectedCodeLeavesTheStoredOneIntact()
    {
        constexpr int size = 3;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("good"), size,
                              pack(QVector<bool>(size * size, true), size)));
        QVERIFY(!store.setCode(QStringLiteral("bad"), size, QByteArray(1, '\0')));

        int modules = 0;
        QCOMPARE(store.gridFor(QStringLiteral("good"), &modules).size(), 3);
        QCOMPARE(modules, size);
    }

    void aProviderWithoutAStoreIsInertRatherThanCrashing()
    {
        QrImageProvider provider(nullptr);
        QSize outSize(7, 7);
        QVERIFY(provider.requestImage(QStringLiteral("t"), &outSize,
                                      QSize(64, 64))
                    .isNull());
        QCOMPARE(outSize, QSize());
    }
};

QTEST_GUILESS_MAIN(QrImageProviderTest)
#include "QrImageProviderTest.moc"
