
#include "calls/GstBootstrap.h"

#include <QtTest/QtTest>

#include <gst/gst.h>

class GstBootstrapTest : public QObject
{
    Q_OBJECT

private slots:
    void theAppImagesOwnGstreamerIsReportedAsBundled()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString bundle = appDir + QStringLiteral("/usr/lib/gstreamer-1.0");

        QCOMPARE(lightning::gst::appImageBundledPluginPath(appDir, bundle,
                                                            QString{}),
                 bundle);
        QCOMPARE(lightning::gst::appImageBundledPluginPath(appDir, QString{},
                                                            bundle),
                 bundle);
        QCOMPARE(lightning::gst::appImageBundledPluginPath(
                     appDir, QStringLiteral("/usr/lib/x86_64-linux-gnu/gstreamer-1.0:")
                                 + bundle,
                     QString{}),
                 bundle);
    }

    void aBundleTheHookNeverPointedAtIsNotReportedAsBundled()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString bundle = appDir + QStringLiteral("/usr/lib/gstreamer-1.0");

        QVERIFY2(lightning::gst::appImageBundledPluginPath(
                     appDir, QStringLiteral("/usr/lib/x86_64-linux-gnu/gstreamer-1.0"),
                     QString{}).isEmpty(),
                 "a bundle GStreamer was never pointed at was reported as the "
                 "loaded one, which is the silent-absence failure the flag "
                 "exists to expose");
        QVERIFY(lightning::gst::appImageBundledPluginPath(
                    QString{}, bundle, bundle).isEmpty());
        QVERIFY2(lightning::gst::appImageBundledPluginPath(
                     appDir, bundle + QStringLiteral("-old"), QString{}).isEmpty(),
                 "a neighbouring directory was accepted as the bundle");
    }

    void theAppImagesOwnPluginScannerIsReported()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString scanner =
            appDir + QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner");

        QCOMPARE(lightning::gst::appImageBundledScannerPath(appDir, scanner,
                                                            QString{}),
                 scanner);
        QCOMPARE(lightning::gst::appImageBundledScannerPath(appDir, QString{},
                                                            scanner),
                 scanner);
        QCOMPARE(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     appDir + QStringLiteral("/usr/bin/../libexec/gstreamer-1.0/gst-plugin-scanner"),
                     QString{}),
                 scanner);
    }

    void aScannerTheHookNeverPointedAtIsNotReported()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString scanner =
            appDir + QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner");

        QVERIFY2(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner"),
                     QString{}).isEmpty(),
                 "a helper GStreamer was never pointed at was reported as the "
                 "one in use");
        QVERIFY(lightning::gst::appImageBundledScannerPath(
                    QString{}, scanner, scanner).isEmpty());
        QVERIFY(lightning::gst::appImageBundledScannerPath(
                    appDir, QString{}, QString{}).isEmpty());
        QVERIFY2(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner:")
                         + scanner,
                     QString{}).isEmpty(),
                 "a path LIST was accepted for a variable that names a single "
                 "executable");
    }

    // On macOS the registry helper is derived from the executable's own
    // directory, never a hardcoded builder path; without it GStreamer falls
    // back to in-process scanning, which works and so hides the omission. This
    // pure half is compiled everywhere and called only on Apple
    // (GstBootstrap.cpp), so the packaging contract is testable anywhere.
    void theScannerPathIsDerivedFromTheExecutablesOwnDirectory()
    {
        const QString macOsDir =
            QStringLiteral("/Applications/Lightning.app/Contents/MacOS");
        QCOMPARE(lightning::gst::scannerPathBesideExecutable(macOsDir),
                 macOsDir + QStringLiteral("/gst-plugin-scanner"));

        const QString elsewhere =
            QStringLiteral("/Users/someone/Downloads/Lightning.app/Contents/MacOS");
        QCOMPARE(lightning::gst::scannerPathBesideExecutable(elsewhere),
                 elsewhere + QStringLiteral("/gst-plugin-scanner"));

        QVERIFY(lightning::gst::scannerPathBesideExecutable(
                    QStringLiteral("MacOS")).startsWith(QLatin1Char('/')));

        // Nothing in, nothing out: never a bare "/gst-plugin-scanner", a path
        // an attacker could plant.
        QVERIFY(lightning::gst::scannerPathBesideExecutable(QString{})
                    .isEmpty());
    }

    void aBuildThatAppliedNoScannerReportsNone()
    {
        QVERIFY(lightning::gst::bundledScannerPath().isEmpty());
    }

    void anExecutableScannerBesideTheBinaryIsApplied()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString scanner = dir.filePath(QStringLiteral("gst-plugin-scanner"));
        QVERIFY(writeExecutable(scanner));
        const EnvGuard guard;

        QVERIFY(lightning::gst::applyBundledScannerPath(dir.path()));
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SCANNER_1_0"), scanner);
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SCANNER"), scanner);
        QCOMPARE(lightning::gst::bundledScannerPath(), scanner);
    }

    // A present but non-executable file (the shape of a mis-signed helper,
    // which Apple Silicon SIGKILLs silently) is refused, leaving GStreamer its
    // working fallback.
    void aScannerThatCannotBeExecutedIsRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString scanner = dir.filePath(QStringLiteral("gst-plugin-scanner"));
        QVERIFY(writeExecutable(scanner,false));
        const EnvGuard guard;

        QVERIFY(!lightning::gst::applyBundledScannerPath(dir.path()));
        QVERIFY(qEnvironmentVariableIsEmpty("GST_PLUGIN_SCANNER_1_0"));
        QVERIFY(qEnvironmentVariableIsEmpty("GST_PLUGIN_SCANNER"));
    }

    void aBundleCarryingNoScannerIsRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const EnvGuard guard;

        QVERIFY(!lightning::gst::applyBundledScannerPath(dir.path()));
        QVERIFY(qEnvironmentVariableIsEmpty("GST_PLUGIN_SCANNER"));
    }

    void anExistingOverrideIsNeverReplaced()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeExecutable(dir.filePath(QStringLiteral("gst-plugin-scanner"))));

        for (const char *name : { "GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0" }) {
            const EnvGuard guard;
            const QByteArray chosen = "/somewhere/else/gst-plugin-scanner";
            qputenv(name, chosen);
            QVERIFY2(!lightning::gst::applyBundledScannerPath(dir.path()),
                     name);
            QCOMPARE(qgetenv(name), chosen);
        }
    }

    void deviceProbeNoiseIsCollapsedButNeverHidden()
    {
        QString whyNot;
        if (!lightning::gst::ensureInitialised(&whyNot))
            QSKIP("GStreamer is unavailable in this environment");

        static QStringList captured;
        captured.clear();
        QtMessageHandler previous = qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &m) {
                captured.append(m);
            });

        const auto probeNoise = [] {
            g_log("GStreamer", G_LOG_LEVEL_CRITICAL, "%s",
                  "range start is not smaller than end for `GstIntRange'");
        };

        probeNoise();
        const int afterFirst = captured.size();
        qInstallMessageHandler(previous);
        QCOMPARE(afterFirst, 1);
        QVERIFY2(captured.at(0).contains(QLatin1String("GstIntRange")),
                 qPrintable(captured.at(0)));
        QVERIFY2(captured.at(0).contains(QLatin1String("not Lightning")),
                 "the first report must say whose problem this is, or the "
                 "next reader spends a round hunting it in src/calls/");

        captured.clear();
        qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &m) {
                captured.append(m);
            });
        for (int i = 0; i < 50; ++i)
            probeNoise();
        const int afterFifty = captured.size();

        g_log("GStreamer", G_LOG_LEVEL_WARNING, "%s",
              "a completely unrelated gstreamer complaint");
        const int afterUnrelated = captured.size();
        qInstallMessageHandler(previous);

        QCOMPARE(afterFifty, 0);
        QCOMPARE(afterUnrelated, 0);
    }

private:
    struct EnvGuard {
        EnvGuard()
            : versioned(qgetenv("GST_PLUGIN_SCANNER_1_0")),
              plain(qgetenv("GST_PLUGIN_SCANNER")),
              hadVersioned(qEnvironmentVariableIsSet("GST_PLUGIN_SCANNER_1_0")),
              hadPlain(qEnvironmentVariableIsSet("GST_PLUGIN_SCANNER"))
        {
            qunsetenv("GST_PLUGIN_SCANNER_1_0");
            qunsetenv("GST_PLUGIN_SCANNER");
        }
        ~EnvGuard()
        {
            hadVersioned ? qputenv("GST_PLUGIN_SCANNER_1_0", versioned)
                         : qunsetenv("GST_PLUGIN_SCANNER_1_0");
            hadPlain ? qputenv("GST_PLUGIN_SCANNER", plain)
                     : qunsetenv("GST_PLUGIN_SCANNER");
        }
        QByteArray versioned;
        QByteArray plain;
        bool hadVersioned;
        bool hadPlain;
    };

    static bool writeExecutable(const QString &path, bool executable = true)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write("#!/bin/sh\nexit 0\n");
        file.close();
        QFile::Permissions perms = QFile::ReadOwner | QFile::WriteOwner;
        if (executable)
            perms |= QFile::ExeOwner;
        return QFile::setPermissions(path, perms);
    }
};

QTEST_MAIN(GstBootstrapTest)
#include "GstBootstrapTest.moc"
