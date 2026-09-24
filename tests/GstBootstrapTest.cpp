// The bundled-plugin rule, with no GStreamer runtime required. A separate
// target because SfuMediaEngineTest QSKIPs its whole suite without the SFU
// runtime, and a skipped suite reports as a pass, so cases there might never
// run; this is pure string handling.

#include "calls/GstBootstrap.h"

#include <QtTest/QtTest>

#include <gst/gst.h>

class GstBootstrapTest : public QObject
{
    Q_OBJECT

private slots:
    // --call-media-status must report the AppImage's own GStreamer as
    // bundled. applyBundledPluginPath looks for `gstreamer-1.0` beside the
    // binary (the Windows/macOS shape); linuxdeploy puts the binary in usr/bin
    // and plugins in usr/lib/gstreamer-1.0, and the AppRun hook exports
    // GST_PLUGIN_SYSTEM_PATH_1_0 at them. The flag exists so a tester's output
    // identifies their runtime.
    void theAppImagesOwnGstreamerIsReportedAsBundled()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString bundle = appDir + QStringLiteral("/usr/lib/gstreamer-1.0");

        QCOMPARE(lightning::gst::appImageBundledPluginPath(appDir, bundle,
                                                            QString{}),
                 bundle);
        // The hook sets both variables; either naming the bundle is enough.
        QCOMPARE(lightning::gst::appImageBundledPluginPath(appDir, QString{},
                                                            bundle),
                 bundle);
        // It is found as one entry of a PATH-shaped list.
        QCOMPARE(lightning::gst::appImageBundledPluginPath(
                     appDir, QStringLiteral("/usr/lib/x86_64-linux-gnu/gstreamer-1.0:")
                                 + bundle,
                     QString{}),
                 bundle);
    }

    // A bundle GStreamer was never pointed at is not reported as bundled; the
    // flag reports what is true.
    void aBundleTheHookNeverPointedAtIsNotReportedAsBundled()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString bundle = appDir + QStringLiteral("/usr/lib/gstreamer-1.0");

        // The hook did not run: neither variable names the bundle.
        QVERIFY2(lightning::gst::appImageBundledPluginPath(
                     appDir, QStringLiteral("/usr/lib/x86_64-linux-gnu/gstreamer-1.0"),
                     QString{}).isEmpty(),
                 "a bundle GStreamer was never pointed at was reported as the "
                 "loaded one, which is the silent-absence failure the flag "
                 "exists to expose");
        // Not an AppImage at all.
        QVERIFY(lightning::gst::appImageBundledPluginPath(
                    QString{}, bundle, bundle).isEmpty());
        // A different directory that merely shares the prefix.
        QVERIFY2(lightning::gst::appImageBundledPluginPath(
                     appDir, bundle + QStringLiteral("-old"), QString{}).isEmpty(),
                 "a neighbouring directory was accepted as the bundle");
    }

    // The AppImage's own gst-plugin-scanner is reported, never set here. The
    // AppRun hook stages and exports it (it lives in usr/libexec/gstreamer-1.0,
    // not beside the binary), and --call-media-status names it.
    void theAppImagesOwnPluginScannerIsReported()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString scanner =
            appDir + QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner");

        QCOMPARE(lightning::gst::appImageBundledScannerPath(appDir, scanner,
                                                            QString{}),
                 scanner);
        // The hook sets both spellings; either is enough.
        QCOMPARE(lightning::gst::appImageBundledScannerPath(appDir, QString{},
                                                            scanner),
                 scanner);
        // Written the long way round, as $APPDIR expansion can produce.
        QCOMPARE(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     appDir + QStringLiteral("/usr/bin/../libexec/gstreamer-1.0/gst-plugin-scanner"),
                     QString{}),
                 scanner);
    }

    // A helper the hook never pointed at is not reported: with in-process
    // scanning as a working fallback, a false "there is a helper" would hide
    // the next packaging regression.
    void aScannerTheHookNeverPointedAtIsNotReported()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString scanner =
            appDir + QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner");

        // The hook did not run: the variables name the host's helper.
        QVERIFY2(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner"),
                     QString{}).isEmpty(),
                 "a helper GStreamer was never pointed at was reported as the "
                 "one in use");
        // Not an AppImage at all.
        QVERIFY(lightning::gst::appImageBundledScannerPath(
                    QString{}, scanner, scanner).isEmpty());
        // Neither variable set: the development case.
        QVERIFY(lightning::gst::appImageBundledScannerPath(
                    appDir, QString{}, QString{}).isEmpty());
        // A colon-joined list is refused, unlike the plugin path:
        // GST_PLUGIN_SCANNER names one executable and GStreamer would run the
        // joined string verbatim.
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
        // The real shape: Lightning.app/Contents/MacOS.
        const QString macOsDir =
            QStringLiteral("/Applications/Lightning.app/Contents/MacOS");
        QCOMPARE(lightning::gst::scannerPathBesideExecutable(macOsDir),
                 macOsDir + QStringLiteral("/gst-plugin-scanner"));

        // It follows the executable wherever the bundle is moved.
        const QString elsewhere =
            QStringLiteral("/Users/someone/Downloads/Lightning.app/Contents/MacOS");
        QCOMPARE(lightning::gst::scannerPathBesideExecutable(elsewhere),
                 elsewhere + QStringLiteral("/gst-plugin-scanner"));

        // A relative directory resolves to an absolute path: GStreamer execs
        // the value, and a Finder launch has "/" as its working directory.
        QVERIFY(lightning::gst::scannerPathBesideExecutable(
                    QStringLiteral("MacOS")).startsWith(QLatin1Char('/')));

        // Nothing in, nothing out: never a bare "/gst-plugin-scanner", a path
        // an attacker could plant.
        QVERIFY(lightning::gst::scannerPathBesideExecutable(QString{})
                    .isEmpty());
    }

    // bundledScannerPath() reports what this process applied. This suite never
    // calls ensureInitialised, so it must be empty on every platform, not the
    // path the layout would use.
    void aBuildThatAppliedNoScannerReportsNone()
    {
        QVERIFY(lightning::gst::bundledScannerPath().isEmpty());
    }

    // ---- applyBundledScannerPath ----
    // Only the call site is Apple-guarded; the logic runs here on Linux. Each
    // case restores the environment, since a leaked GST_PLUGIN_SCANNER would
    // change what later cases see.

    void anExecutableScannerBesideTheBinaryIsApplied()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString scanner = dir.filePath(QStringLiteral("gst-plugin-scanner"));
        QVERIFY(writeExecutable(scanner));
        const EnvGuard guard;

        QVERIFY(lightning::gst::applyBundledScannerPath(dir.path()));
        // Both spellings: GStreamer reads the versioned name first.
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
        QVERIFY(writeExecutable(scanner, /*executable=*/false));
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

    // An existing override wins under either spelling, or it would be honoured
    // only half the time depending on GStreamer's lookup order.
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

    /// GStreamer's device providers probe every ALSA PCM, and a device with a
    /// degenerate rate range makes GLib print a CRITICAL pair per probe (the
    /// stock gst-device-monitor-1.0 does the same). The noise is collapsed, not
    /// suppressed: the first always reaches the log, since this project can
    /// produce bad caps ranges of its own, and unrelated GStreamer messages are
    /// untouched.
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

        // The first is reported in full.
        probeNoise();
        const int afterFirst = captured.size();
        qInstallMessageHandler(previous);
        QCOMPARE(afterFirst, 1);
        QVERIFY2(captured.at(0).contains(QLatin1String("GstIntRange")),
                 qPrintable(captured.at(0)));
        QVERIFY2(captured.at(0).contains(QLatin1String("not Lightning")),
                 "the first report must say whose problem this is, or the "
                 "next reader spends a round hunting it in src/calls/");

        // The following ones do not each produce a line.
        captured.clear();
        qInstallMessageHandler(
            [](QtMsgType, const QMessageLogContext &, const QString &m) {
                captured.append(m);
            });
        for (int i = 0; i < 50; ++i)
            probeNoise();
        const int afterFifty = captured.size();

        // An unrelated message passes straight through.
        g_log("GStreamer", G_LOG_LEVEL_WARNING, "%s",
              "a completely unrelated gstreamer complaint");
        const int afterUnrelated = captured.size();
        qInstallMessageHandler(previous);

        QCOMPARE(afterFifty, 0);
        QCOMPARE(afterUnrelated, 0);   // forwarded to GLib, not to Qt
    }

private:
    // Saves and restores both spellings.
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
