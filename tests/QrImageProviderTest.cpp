
#include "crypto/QrImageProvider.h"

#include <QImage>
#include <QtTest>

namespace {

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

}

class QrImageProviderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void gridModulesLandWhereTheGridSaysAndKeepAQuietZone()
    {
        constexpr int size = 3;
        QVector<bool> modules(size * size, false);
        modules[0] = true;
        modules[2] = true;
        modules[size * 2] = true;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("tok"), size,
                              pack(modules, size)));

        QrImageProvider provider(&store);
        QSize outSize;
        const int total = size + 2 * kQuiet;
        const QImage img = provider.requestImage(QStringLiteral("tok"),
                                                 &outSize, QSize(total, total));
        QCOMPARE(img.size(), QSize(total, total));
        QCOMPARE(outSize, QSize(total, total));

        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 0), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 2, kQuiet + 0), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 2), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 1, kQuiet + 1), QColor(Qt::white));
        QCOMPARE(img.pixelColor(kQuiet + 2, kQuiet + 2), QColor(Qt::white));

        for (int i = 0; i < total; ++i) {
            QCOMPARE(img.pixelColor(i, 0), QColor(Qt::white));
            QCOMPARE(img.pixelColor(0, i), QColor(Qt::white));
            QCOMPARE(img.pixelColor(i, total - 1), QColor(Qt::white));
            QCOMPARE(img.pixelColor(total - 1, i), QColor(Qt::white));
        }
    }

    void rowsWiderThanAByteDoNotShear()
    {
        constexpr int size = 9;
        QVector<bool> modules(size * size, false);
        modules[8] = true;
        modules[size * 1 + 0] = true;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("t"), size, pack(modules, size)));

        QrImageProvider provider(&store);
        const int total = size + 2 * kQuiet;
        const QImage img = provider.requestImage(QStringLiteral("t"), nullptr,
                                                 QSize(total, total));
        QCOMPARE(img.pixelColor(kQuiet + 8, kQuiet + 0), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 1), QColor(Qt::black));
        QCOMPARE(img.pixelColor(kQuiet + 0, kQuiet + 0), QColor(Qt::white));
        QCOMPARE(img.pixelColor(kQuiet + 8, kQuiet + 1), QColor(Qt::white));
    }

    void scalingStaysOnWholeModules()
    {
        constexpr int size = 5;
        QrCodeStore store;
        QVERIFY(store.setCode(QStringLiteral("s"), size,
                              pack(QVector<bool>(size * size, true), size)));
        QrImageProvider provider(&store);
        const int total = size + 2 * kQuiet;

        const QImage img = provider.requestImage(QStringLiteral("s"), nullptr,
                                                 QSize(100, 100));
        QCOMPARE(img.width() % total, 0);
        QCOMPARE(img.height(), img.width());
        QCOMPARE(img.width(), total * (100 / total));

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
        QVERIFY(!provider.requestImage(QStringLiteral("current"), nullptr,
                                       QSize(64, 64))
                     .isNull());
    }

    void malformedGeometryIsRefusedByTheStore()
    {
        QrCodeStore store;
        QVERIFY(!store.setCode(QStringLiteral("t"), 3, QByteArray(2, '\0')));
        QVERIFY(!store.setCode(QStringLiteral("t"), 0, QByteArray()));
        QVERIFY(!store.setCode(QStringLiteral("t"), -1, QByteArray()));
        const int tooBig = QrCodeStore::kMaxModules + 1;
        QVERIFY(!store.setCode(QStringLiteral("t"), tooBig,
                               QByteArray(((tooBig + 7) / 8) * tooBig, '\0')));
        QVERIFY(!store.setCode(QString{}, 3, QByteArray(3, '\0')));

        int modules = -1;
        QVERIFY(store.gridFor(QStringLiteral("t"), &modules).isEmpty());
        QCOMPARE(modules, 0);
    }

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
