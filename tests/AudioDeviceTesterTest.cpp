// The Settings microphone and speaker test (AudioDeviceTester).
//
// The state machine (refusals, the time limits, a call taking over, teardown)
// and, with the media engine, the pipelines it actually hands GStreamer: test
// sources and fakesinks stand in for the devices, but the descriptions are
// the production ones, parsed and run. A description this test composed
// itself would prove nothing about the one the tester builds.

#include <QSignalSpy>
#include <QtTest/QtTest>

#include "calls/AudioDeviceTester.h"

#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstBootstrap.h"
#include "calls/SfuMediaEngine.h"

#include <gst/gst.h>
#endif

namespace {

#ifdef HAVE_LIGHTNING_WEBRTC
// GStreamer with `level`, or the case is skipped (the engine's own suite
// skips the same way).
bool gstreamerWithMeter()
{
    if (!lightning::gst::ensureInitialised())
        return false;
    GstElementFactory *factory = gst_element_factory_find("level");
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

// The synthetic microphone the tester's test mode opens.
const QString kTestMicrophone = QStringLiteral(
    "audiotestsrc is-live=true wave=sine freq=440 volume=0.5 name=micsrc");

// Parses `description` as GStreamer would and reports its error, if any.
QString parseError(const QString &description)
{
    GError *error = nullptr;
    GstElement *pipeline =
        gst_parse_launch(description.toUtf8().constData(), &error);
    QString message;
    if (error) {
        message = QString::fromUtf8(error->message ? error->message : "?");
        g_error_free(error);
    } else if (!pipeline) {
        message = QStringLiteral("no pipeline");
    }
    if (pipeline)
        gst_object_unref(pipeline);
    return message;
}
#endif

} // namespace

class AudioDeviceTesterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Until AppController has found the media runtime, nothing starts: tests
    // and builds without it must never open a device.
    void nothingStartsBeforeTheRuntimeIsKnown()
    {
        AudioDeviceTester tester;
        QVERIFY(!tester.available());
        QSignalSpy modes(&tester, &AudioDeviceTester::modeChanged);
        QVERIFY(!tester.startMicrophoneTest(true));
        QCOMPARE(tester.error(), QStringLiteral("unavailable"));
        QVERIFY(!tester.playTestSound());
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Idle);
        QCOMPARE(modes.count(), 0);
#ifndef HAVE_LIGHTNING_WEBRTC
        // Without the engine no runtime can make it available.
        QVERIFY(!tester.built());
        tester.setRuntimeAvailable(true);
        QVERIFY(!tester.available());
#else
        QVERIFY(tester.built());
#endif
    }

    // The call owns the microphone: a test is refused while one is ringing,
    // starting or live, and says so.
    void aTestIsRefusedDuringACall()
    {
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setRuntimeAvailable(true);
        QSignalSpy blocked(&tester, &AudioDeviceTester::blockedByCallChanged);
        tester.setCallActive(true);
        QCOMPARE(blocked.count(), 1);
        QVERIFY(tester.blockedByCall());
        QVERIFY(!tester.startMicrophoneTest(false));
        QCOMPARE(tester.error(), QStringLiteral("in_call"));
        QVERIFY(!tester.startMicrophoneTest(true));
        QVERIFY(!tester.playTestSound());
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Idle);
        QVERIFY(!tester.pipelineRunningForTest());
    }

#ifdef HAVE_LIGHTNING_WEBRTC
    // The level check runs what it describes: the call's own capture front,
    // verbatim, into a meter and a fakesink, and the meter moves.
    void theLevelCheckRunsTheCallsOwnChainAndReportsALevel()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setRuntimeAvailable(true);
        QSignalSpy levels(&tester, &AudioDeviceTester::levelChanged);

        QVERIFY(tester.startMicrophoneTest(false));
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Meter);
        QCOMPARE(tester.modeName(), QStringLiteral("meter"));
        QVERIFY(tester.microphoneTestRunning());
        QTRY_VERIFY_WITH_TIMEOUT(tester.pipelineRunningForTest(), 8000);
        QVERIFY2(tester.error().isEmpty(), qPrintable(tester.error()));

        const QString built = tester.lastDescriptionForTest();
        SfuMediaEngine::MicrophoneCapture capture;
        capture.source = kTestMicrophone;
        QVERIFY2(built.contains(SfuMediaEngine::microphoneFrontDescription(capture)),
                 qPrintable(built));
        QVERIFY(built.contains(QStringLiteral("level name=testlevel")));
        QVERIFY(built.contains(QStringLiteral("volume name=testvol")));
        // Meter only: nothing is played anywhere.
        QVERIFY(!built.contains(QStringLiteral("outsink")));

        // A -6 dBFS sine (whatever the voice processing makes of it) lights
        // the bar.
        QTRY_VERIFY_WITH_TIMEOUT(tester.level() > 0.1, 5000);

        // A second of readings stays within the meter's rate.
        levels.clear();
        QTest::qWait(1000);
        QVERIFY2(levels.count()
                     <= 1000 / lightning::calls::kMeterMinIntervalMs + 2,
                 qPrintable(QString::number(levels.count())));

        tester.stop();
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Idle);
        QCOMPARE(tester.level(), 0.0);
        QVERIFY(!tester.pipelineRunningForTest());
        // A reading still in flight for the ended session changes nothing.
        tester.noteLevelForTest(-3.0, 10'000'000);
        QCOMPARE(tester.level(), 0.0);
    }

    // "Let's check" plays the voice back through a bounded, leaky queue (a
    // default queue would replay a growing echo) and stops by itself.
    void theVoicePlaybackIsBoundedAndStopsByItself()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setRuntimeAvailable(true);
        tester.setLimitMsForTest(1500);

        QVERIFY(tester.startMicrophoneTest(true));
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Loopback);
        QVERIFY(tester.secondsLeft() > 0);
        QTRY_VERIFY_WITH_TIMEOUT(tester.pipelineRunningForTest(), 8000);
        const QString built = tester.lastDescriptionForTest();
        QVERIFY2(built.contains(QStringLiteral("name=outsink")), qPrintable(built));
        const int queue = built.lastIndexOf(QStringLiteral("! queue "));
        QVERIFY2(queue > built.indexOf(QStringLiteral("testlevel")),
                 qPrintable(built));
        const QString playbackQueue = built.mid(queue).section(QLatin1Char('!'), 1, 1);
        QVERIFY2(playbackQueue.contains(QStringLiteral("leaky=downstream")),
                 qPrintable(playbackQueue));
        QVERIFY2(playbackQueue.contains(QStringLiteral("max-size-time=200000000")),
                 qPrintable(playbackQueue));

        // The limit ends it with no error and nothing left running.
        QTRY_COMPARE_WITH_TIMEOUT(tester.mode(), AudioDeviceTester::Mode::Idle,
                                  5000);
        QVERIFY(tester.error().isEmpty());
        QCOMPARE(tester.secondsLeft(), 0);
        QVERIFY(!tester.pipelineRunningForTest());
    }

    // A multi-input interface is opened as a call opens it: input 1 by
    // mix-matrix, pinned and unpositioned. Parses AND runs (a wrong pin fails
    // negotiation, which only running shows).
    void aMultiInputMicrophoneIsTestedAsACallOpensIt()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setDeviceChannelsForTest(4);
        tester.setRuntimeAvailable(true);

        QVERIFY(tester.startMicrophoneTest(false));
        QTRY_VERIFY_WITH_TIMEOUT(tester.pipelineRunningForTest(), 8000);
        const QString built = tester.lastDescriptionForTest();
        QVERIFY2(built.contains(QStringLiteral("mix-matrix")), qPrintable(built));
        QVERIFY2(built.contains(
                     QStringLiteral("channels=4,channel-mask=(bitmask)0x0")),
                 qPrintable(built));
        QTRY_VERIFY_WITH_TIMEOUT(tester.level() > 0.1, 5000);
        QVERIFY2(tester.error().isEmpty(), qPrintable(tester.error()));
        tester.stop();
    }

    // The output test is a short tone that ends by itself (EOS), without an
    // error and without waiting for the backstop.
    void theTestToneEndsByItself()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setRuntimeAvailable(true);

        QVERIFY(tester.playTestSound());
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Tone);
        QVERIFY(tester.toneRunning());
        QVERIFY(!tester.microphoneTestRunning());
        QTRY_COMPARE_WITH_TIMEOUT(tester.mode(), AudioDeviceTester::Mode::Idle,
                                  4000);
        QVERIFY2(tester.error().isEmpty(), qPrintable(tester.error()));
        const QString built = tester.lastDescriptionForTest();
        QVERIFY2(built.contains(QStringLiteral("num-buffers=%1")
                                    .arg(AudioDeviceTester::kToneBuffers)),
                 qPrintable(built));
        QVERIFY(built.contains(QStringLiteral("name=outsink")));
        // No microphone is opened for the tone.
        QVERIFY(!built.contains(QStringLiteral("micsrc")));
    }

    // A call starting ends a running test at once.
    void aCallStartingStopsARunningTest()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setRuntimeAvailable(true);
        QVERIFY(tester.startMicrophoneTest(true));
        QTRY_VERIFY_WITH_TIMEOUT(tester.pipelineRunningForTest(), 8000);

        tester.setCallActive(true);
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Idle);
        QVERIFY(!tester.pipelineRunningForTest());
        QCOMPARE(tester.level(), 0.0);
        // Not an error: the call simply has the microphone now.
        QVERIFY(tester.error().isEmpty());
    }

    // Starting another test replaces the running one; the old session's
    // late readings are discarded.
    void aNewTestReplacesTheRunningOne()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setRuntimeAvailable(true);
        QVERIFY(tester.startMicrophoneTest(false));
        QTRY_VERIFY_WITH_TIMEOUT(tester.pipelineRunningForTest(), 8000);
        QVERIFY(tester.playTestSound());
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Tone);
        QCOMPARE(tester.level(), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(tester.mode(), AudioDeviceTester::Mode::Idle,
                                  4000);
    }

    // Destroying a tester mid-test (the application quitting) tears the
    // pipeline down and delivers nothing to the dead object.
    void destroyingARunningTesterIsSafe()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        auto *tester = new AudioDeviceTester;
        tester->setTestSourceMode(true);
        tester->setRuntimeAvailable(true);
        QVERIFY(tester->startMicrophoneTest(true));
        QTRY_VERIFY_WITH_TIMEOUT(tester->pipelineRunningForTest(), 8000);
        delete tester;
        // Readings queued before the delete must find no receiver.
        QTest::qWait(300);
    }

    // The descriptions a real device produces (not the test sources) parse:
    // the platform-default elements every package carries.
    void theRealDeviceDescriptionsParse()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        for (const auto mode : {AudioDeviceTester::Mode::Meter,
                                AudioDeviceTester::Mode::Loopback,
                                AudioDeviceTester::Mode::Tone}) {
            const QString description = AudioDeviceTester::describeForTest(
                mode, QStringLiteral("autoaudiosrc name=micsrc"), 0,
                QStringLiteral("autoaudiosink name=outsink"), 150);
            QVERIFY(!description.isEmpty());
            // The voice processing stage is in what is parsed whenever this
            // build has it: its caps directly after the front's caps were a
            // parse error ("no element \"audio\"") that only a description
            // WITH the stage shows.
            if (mode != AudioDeviceTester::Mode::Tone) {
                const QString processing =
                    SfuMediaEngine::voiceProcessingDescription(
                        calls::noise::kDefaultMode);
                QVERIFY(processing.isEmpty() || description.contains(processing));
            }
            // The voice playback is not synced to the capture clock: synced,
            // its delay varied 450-1880 ms run to run (GUI rig, 2026-10-06).
            if (mode == AudioDeviceTester::Mode::Loopback) {
                QVERIFY2(description.endsWith(
                             QStringLiteral("autoaudiosink name=outsink sync=false")),
                         qPrintable(description));
            }
            const QString error = parseError(description);
            QVERIFY2(error.isEmpty(),
                     qPrintable(error + QStringLiteral(" in ") + description));
        }
        // The gain reaches the volume element on the call's own scale.
        const QString loud = AudioDeviceTester::describeForTest(
            AudioDeviceTester::Mode::Meter,
            QStringLiteral("autoaudiosrc name=micsrc"), 0, QString(), 200);
        QVERIFY2(loud.contains(QStringLiteral("volume=%1").arg(
                     QString::number(SfuMediaEngine::audioFactorPercent(200)
                                         / 100.0,
                                     'f', 3))),
                 qPrintable(loud));
    }

    // GitHub #20: the microphone test runs the noise suppression the call
    // would, so "Let's check" plays back what others will hear. Before the
    // selector the test could only ever run webrtcdsp's own suppressor.
    void theMicrophoneTestRunsTheSelectedNoiseSuppression()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        GstElementFactory *dsp = gst_element_factory_find("webrtcdsp");
        const bool haveDsp = dsp != nullptr;
        if (dsp)
            gst_object_unref(dsp);
        for (const calls::noise::Mode noise :
             {calls::noise::Mode::Off, calls::noise::Mode::WebRtc,
              calls::noise::Mode::RNNoise, calls::noise::Mode::DeepFilterNet}) {
            const QString description = AudioDeviceTester::describeForTest(
                AudioDeviceTester::Mode::Loopback,
                QStringLiteral("autoaudiosrc name=micsrc"), 0,
                QStringLiteral("autoaudiosink name=outsink"), 100, noise);
            QVERIFY2(description.contains(
                         SfuMediaEngine::voiceProcessingDescription(noise)),
                     qPrintable(description));
            QVERIFY2(description.contains(
                         QStringLiteral("name=micdenoise mode=%1")
                             .arg(QLatin1String(calls::noise::modeKey(noise)))),
                     qPrintable(description));
            if (haveDsp) {
                QCOMPARE(description.contains(
                             QStringLiteral("noise-suppression=true")),
                         noise == calls::noise::Mode::WebRtc);
            }
            const QString error = parseError(description);
            QVERIFY2(error.isEmpty(),
                     qPrintable(error + QStringLiteral(" in ") + description));
        }
    }
#endif
};

QTEST_GUILESS_MAIN(AudioDeviceTesterTest)
#include "AudioDeviceTesterTest.moc"
