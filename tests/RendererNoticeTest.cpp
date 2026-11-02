#include <QFile>
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
        QVERIFY(shouldShowSoftwareRendererNotice(llvm, "", "", "0.10.0",
                                                 "0.10.1"));
        QVERIFY(!shouldShowSoftwareRendererNotice("NVIDIA GeForce", "", "", "",
                                                  "0.10.0"));
    }

    void theNoticeNamesOnlyThePlatformItIsOn()
    {
        QVERIFY(isNixOsProductType(QStringLiteral("nixos")));
        QVERIFY(isNixOsProductType(QStringLiteral("NixOS")));
        QVERIFY(!isNixOsProductType(QStringLiteral("debian")));
        QVERIFY(!isNixOsProductType(QStringLiteral("fedora")));
        QVERIFY(!isNixOsProductType(QString()));

        QCOMPARE(softwareRendererContextId(softwareRendererContext(true, true)),
                 QStringLiteral("appimage-nixos"));
        QCOMPARE(softwareRendererContextId(softwareRendererContext(true, false)),
                 QStringLiteral("appimage"));
        QCOMPARE(softwareRendererContextId(softwareRendererContext(false, true)),
                 QStringLiteral("nixos"));
        QCOMPARE(softwareRendererContextId(softwareRendererContext(false, false)),
                 QStringLiteral("generic"));
    }

    void theQmlWordsTheNoticeFromTheContext()
    {
        QFile file(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/Main.qml"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable(file.fileName()));
        const QString qml = QString::fromUtf8(file.readAll());
        const qsizetype start = qml.indexOf(
            QStringLiteral("objectName: \"softwareRendererNotice\""));
        QVERIFY(start >= 0);
        const qsizetype end = qml.indexOf(
            QStringLiteral("objectName: \"softwareRendererNoticeDismissButton\""),
            start);
        QVERIFY(end > start);
        const QString notice = qml.mid(start, end - start);

        QVERIFY2(notice.contains(QStringLiteral("app.softwareRendererNoticeContext")),
                 "the notice must read the platform context");
        const QString nixAppImage = QStringLiteral("with the AppImage on NixOS");
        QCOMPARE(notice.count(nixAppImage), 1);
        const qsizetype branch = notice.indexOf(QStringLiteral("case \"appimage-nixos\":"));
        QVERIFY(branch >= 0);
        const qsizetype sentence = notice.indexOf(nixAppImage);
        QVERIFY(sentence > branch);
        QVERIFY(notice.indexOf(QStringLiteral("case "), branch + 1) > sentence);
        const qsizetype fallback = notice.indexOf(QStringLiteral("default:"));
        QVERIFY(fallback > 0);
        const QString generic = notice.mid(fallback, notice.indexOf(QLatin1Char('}'), fallback) - fallback);
        QVERIFY(!generic.contains(QStringLiteral("NixOS")));
        QVERIFY(!generic.contains(QStringLiteral("AppImage")));
        const qsizetype help = notice.indexOf(
            QStringLiteral("objectName: \"softwareRendererNoticeHelpButton\""));
        QVERIFY(help > 0);
        QVERIFY(notice.mid(help, 400).contains(QStringLiteral(
            "visible: softwareRendererNotice.noticeContext === \"appimage-nixos\"")));
    }
};

QTEST_APPLESS_MAIN(RendererNoticeTest)
#include "RendererNoticeTest.moc"
