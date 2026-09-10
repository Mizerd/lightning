// The bundled-plugin rule, with no GStreamer runtime required.
//
// These cases live in their own target on purpose. SfuMediaEngineTest, the
// obvious home, opens with a QSKIP of the WHOLE suite when the SFU runtime is
// unavailable — and a skipped test reports as a pass through ctest, so a case
// placed there could quietly never run. That is a recorded trap in this
// project ("a test can fail to RUN at all"), and the rule under test here is
// pure string handling that needs no runtime at all.

#include "calls/GstBootstrap.h"

#include <QtTest/QtTest>

class GstBootstrapTest : public QObject
{
    Q_OBJECT

private slots:
    // --call-media-status MISNAMED THE RUNTIME ON THE ONE LINUX PACKAGE THAT
    // BUNDLES GSTREAMER.
    //
    // Found on 2026-09-08 by running the CI-built AppImage from pipeline 181
    // in a clean debian:13.6-slim with ZERO gstreamer packages installed. It
    // ran, both engines reported available, and GStreamer scanned the
    // bundle's own plugins (the container's warnings name
    // `.../usr/lib/gstreamer-1.0/libgstalsa.so`) -- while the status flag
    // said `bundled= false` and `<none - using system GStreamer>`.
    //
    // The cause is a layout assumption: applyBundledPluginPath looks for
    // `gstreamer-1.0` BESIDE the binary, which is the Windows and macOS
    // shape. linuxdeploy puts the binary in `usr/bin` and the plugins in
    // `usr/lib/gstreamer-1.0`, and the AppRun hook exports
    // GST_PLUGIN_SYSTEM_PATH_1_0 at them before the process starts.
    //
    // It matters because that flag exists so a tester's output identifies
    // their runtime without a round trip, and four packaging defects in this
    // project have turned on exactly which GStreamer was loaded.
    void theAppImagesOwnGstreamerIsReportedAsBundled()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString bundle = appDir + QStringLiteral("/usr/lib/gstreamer-1.0");

        QCOMPARE(lightning::gst::appImageBundledPluginPath(appDir, bundle,
                                                            QString{}),
                 bundle);
        // The hook sets both; either one naming it is enough.
        QCOMPARE(lightning::gst::appImageBundledPluginPath(appDir, QString{},
                                                            bundle),
                 bundle);
        // And it survives being one entry among several, which is how a
        // PATH-shaped variable is legitimately written.
        QCOMPARE(lightning::gst::appImageBundledPluginPath(
                     appDir, QStringLiteral("/usr/lib/x86_64-linux-gnu/gstreamer-1.0:")
                                 + bundle,
                     QString{}),
                 bundle);
    }

    // ...AND IT MUST NOT CLAIM A BUNDLE THAT IS NOT IN USE.
    //
    // The whole value of the flag is that it reports what is TRUE. A payload
    // that exists but that GStreamer was never pointed at is exactly the
    // silent-absence failure this project has hit four times, and answering
    // "bundled" for it would hide the next one.
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
        // A DIFFERENT directory that merely starts with the same prefix.
        QVERIFY2(lightning::gst::appImageBundledPluginPath(
                     appDir, bundle + QStringLiteral("-old"), QString{}).isEmpty(),
                 "a neighbouring directory was accepted as the bundle");
    }

    // THE APPIMAGE'S HELPER IS REPORTED, AND NEVER SET FROM HERE.
    //
    // 0.9.4 shipped an AppImage that printed "External plugin loader failed"
    // at every launch: it bundles its own GStreamer, whose compiled-in libexec
    // path names the BUILD IMAGE. The AppRun hook now stages and exports the
    // helper, exactly as it already does for the plugin path — the helper sits
    // at usr/libexec/gstreamer-1.0/, not beside the binary at usr/bin/, so the
    // macOS "beside the executable" rule cannot reach it — and this is the
    // half that NOTICES, so --call-media-status names the helper instead of
    // reporting GStreamer's own.
    void theAppImagesOwnPluginScannerIsReported()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString scanner =
            appDir + QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner");

        QCOMPARE(lightning::gst::appImageBundledScannerPath(appDir, scanner,
                                                            QString{}),
                 scanner);
        // The hook sets both spellings; either one naming it is enough.
        QCOMPARE(lightning::gst::appImageBundledScannerPath(appDir, QString{},
                                                            scanner),
                 scanner);
        // Written the long way round, which is what $APPDIR expansion in a
        // shell hook can legitimately produce.
        QCOMPARE(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     appDir + QStringLiteral("/usr/bin/../libexec/gstreamer-1.0/gst-plugin-scanner"),
                     QString{}),
                 scanner);
    }

    // ...AND IT MUST NOT CLAIM ONE THAT IS NOT IN USE.
    //
    // Same rule as the plugin path, and it is load-bearing for the same
    // reason: graceful fallback and silent absence are the same observable,
    // and a status line that says "there is a helper" when GStreamer is
    // scanning in-process would hide the next packaging regression rather
    // than expose it.
    void aScannerTheHookNeverPointedAtIsNotReported()
    {
        const QString appDir = QStringLiteral("/tmp/appimage_extracted_abc");
        const QString scanner =
            appDir + QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner");

        // The hook did not run: the variables still name the host's helper.
        QVERIFY2(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner"),
                     QString{}).isEmpty(),
                 "a helper GStreamer was never pointed at was reported as the "
                 "one in use");
        // Not an AppImage at all.
        QVERIFY(lightning::gst::appImageBundledScannerPath(
                    QString{}, scanner, scanner).isEmpty());
        // Neither variable set: the ordinary development case.
        QVERIFY(lightning::gst::appImageBundledScannerPath(
                    appDir, QString{}, QString{}).isEmpty());
        // A COLON-JOINED LIST IS REFUSED, and this is the one place the rule
        // differs from its plugin-path sibling. GST_PLUGIN_SCANNER names one
        // EXECUTABLE; GStreamer would try to run the joined string verbatim,
        // so accepting a list here would report a path that cannot run.
        QVERIFY2(lightning::gst::appImageBundledScannerPath(
                     appDir,
                     QStringLiteral("/usr/libexec/gstreamer-1.0/gst-plugin-scanner:")
                         + scanner,
                     QString{}).isEmpty(),
                 "a path LIST was accepted for a variable that names a single "
                 "executable");
    }

    // THE REGISTRY HELPER IS DERIVED, NEVER WRITTEN DOWN.
    //
    // The macOS bundle shipped with no `gst-plugin-scanner`, so every launch
    // printed "External plugin loader failed" and GStreamer fell back to
    // scanning plugins in-process. That fallback works — which is exactly
    // why nothing caught it, and why the maintainer's call log carried a
    // GStreamer warning that had nothing to do with the failure underneath
    // it. The fix must come from the app's OWN location: a hardcoded path is
    // the builder's `libexec`, which is the same assumption that made
    // GST_PLUGIN_PATH necessary in the first place.
    //
    // This is the pure half of that rule. It is compiled on every platform
    // and CALLED only on Apple (GstBootstrap.cpp), so the contract the macOS
    // packaging script has to satisfy is testable on a machine that has no
    // Mac in it.
    void theScannerPathIsDerivedFromTheExecutablesOwnDirectory()
    {
        // The real shape: Lightning.app/Contents/MacOS.
        const QString macOsDir =
            QStringLiteral("/Applications/Lightning.app/Contents/MacOS");
        QCOMPARE(lightning::gst::scannerPathBesideExecutable(macOsDir),
                 macOsDir + QStringLiteral("/gst-plugin-scanner"));

        // It follows the executable wherever the bundle is, which is the
        // whole point — a user drags the app anywhere and a signed, quoted
        // path from the build machine would be dead.
        const QString elsewhere =
            QStringLiteral("/Users/someone/Downloads/Lightning.app/Contents/MacOS");
        QCOMPARE(lightning::gst::scannerPathBesideExecutable(elsewhere),
                 elsewhere + QStringLiteral("/gst-plugin-scanner"));

        // A relative directory is still resolved to an absolute path: the
        // value goes into an environment variable that GStreamer execs, and
        // it must not depend on the working directory the app was started
        // from (a Finder launch has "/" for one).
        QVERIFY(lightning::gst::scannerPathBesideExecutable(
                    QStringLiteral("MacOS")).startsWith(QLatin1Char('/')));

        // Nothing in, nothing out — never a bare "/gst-plugin-scanner",
        // which is a real path an attacker could plant on a system that
        // permits it.
        QVERIFY(lightning::gst::scannerPathBesideExecutable(QString{})
                    .isEmpty());
    }

    // ...AND A DEVELOPMENT BUILD MUST NOT CLAIM ONE.
    //
    // `bundledScannerPath()` reports what this process actually applied,
    // so that `--call-media-status` states a fact rather than a layout.
    // This suite never calls `ensureInitialised`, so nothing was applied and
    // the answer must be empty on every platform. Returning the path the
    // layout WOULD use is the same silent-absence lie that made the AppImage
    // report `bundled= false` while running on its own GStreamer.
    void aBuildThatAppliedNoScannerReportsNone()
    {
        QVERIFY(lightning::gst::bundledScannerPath().isEmpty());
    }

    // ---- applyBundledScannerPath ------------------------------------------
    //
    // Only the CALL SITE is Apple-guarded; the body is ordinary logic, and
    // while it stayed file-local that logic had no coverage on any platform.
    // These run on Linux and would have run on a Mac too.
    //
    // Each case restores the environment itself: the function writes process
    // env, and a leaked GST_PLUGIN_SCANNER would silently change what every
    // later case (and the rest of this binary) sees.

    void anExecutableScannerBesideTheBinaryIsApplied()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString scanner = dir.filePath(QStringLiteral("gst-plugin-scanner"));
        QVERIFY(writeExecutable(scanner));
        const EnvGuard guard;

        QVERIFY(lightning::gst::applyBundledScannerPath(dir.path()));
        // BOTH spellings, because GStreamer reads the versioned name first
        // and falls back to the plain one.
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SCANNER_1_0"), scanner);
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SCANNER"), scanner);
        QCOMPARE(lightning::gst::bundledScannerPath(), scanner);
    }

    // A file that is THERE but cannot be run is the shape a mis-signed helper
    // takes: on Apple Silicon the kernel SIGKILLs it with no message, which
    // reads exactly like the missing-file case. Refusing it leaves GStreamer
    // its own working fallback instead of pointing it at something dead.
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

    // Someone debugging a packaged build has said what they want, and it
    // wins — under EITHER spelling, or the override is honoured only half
    // the time and which half depends on GStreamer's own lookup order.
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

private:
    // Saves and restores both spellings, whatever the case does to them.
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
