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
};

QTEST_MAIN(GstBootstrapTest)
#include "GstBootstrapTest.moc"
