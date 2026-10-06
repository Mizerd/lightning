#include <QtTest>

#include "app/RendererNotice.h"

using namespace lightning;

class RendererNoticeTest : public QObject
{
    Q_OBJECT
private slots:
    void knownSoftwareRasterizersAreDetected_data()
    {
        QTest::addColumn<QString>("renderer");
        QTest::newRow("llvmpipe") << "llvmpipe (LLVM 19.1.7, 256 bits)";
        QTest::newRow("softpipe") << "softpipe";
        QTest::newRow("swiftshader") << "SwiftShader Device (Subzero)";
        QTest::newRow("software rasterizer") << "Software Rasterizer";
        QTest::newRow("mesa upper case") << "LLVMPIPE";
        QTest::newRow("microsoft") << "Microsoft Basic Render Driver";
    }
    void knownSoftwareRasterizersAreDetected()
    {
        QFETCH(QString, renderer);
        QVERIFY(isSoftwareRasterizerRenderer(renderer));
    }

    void realGpusAreNotSoftware_data()
    {
        QTest::addColumn<QString>("renderer");
        QTest::newRow("nvidia") << "NVIDIA GeForce RTX 4070/PCIe/SSE2";
        QTest::newRow("radeon") << "AMD Radeon RX 7800 XT (radeonsi, navi32)";
        QTest::newRow("intel") << "Mesa Intel(R) UHD Graphics 630 (CFL GT2)";
        QTest::newRow("apple") << "Apple M2";
        QTest::newRow("empty") << "";
        QTest::newRow("unknown marker") << "?";
    }
    void realGpusAreNotSoftware()
    {
        QFETCH(QString, renderer);
        QVERIFY(!isSoftwareRasterizerRenderer(renderer));
    }

    void choosingSoftwareOnPurposeSilencesTheNotice()
    {
        const QString llvm = QStringLiteral("llvmpipe (LLVM 19)");
        QVERIFY(softwareRenderingChosenByUser("software", ""));
        QVERIFY(softwareRenderingChosenByUser("", "Software"));
        QVERIFY(!softwareRenderingChosenByUser("", ""));
        QVERIFY(!softwareRenderingChosenByUser("rhi", "opengl"));
        QVERIFY(shouldShowSoftwareRendererNotice(llvm, "", "", "", "0.10.0"));
        QVERIFY(!shouldShowSoftwareRendererNotice(llvm, "software", "", "",
                                                  "0.10.0"));
        QVERIFY(!shouldShowSoftwareRendererNotice(llvm, "", "software", "",
                                                  "0.10.0"));
    }

    void dismissalIsPerVersion()
    {
        const QString llvm = QStringLiteral("llvmpipe");
        QVERIFY(!shouldShowSoftwareRendererNotice(llvm, "", "", "0.10.0",
                                                  "0.10.0"));
        // A newer version asks again.
        QVERIFY(shouldShowSoftwareRendererNotice(llvm, "", "", "0.10.0",
                                                 "0.10.1"));
        // A GPU never asks, whatever was stored.
        QVERIFY(!shouldShowSoftwareRendererNotice("NVIDIA GeForce", "", "", "",
                                                  "0.10.0"));
    }
};

QTEST_APPLESS_MAIN(RendererNoticeTest)
#include "RendererNoticeTest.moc"
