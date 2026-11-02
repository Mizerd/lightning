
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

const QString kTestMicrophone = QStringLiteral(
    "audiotestsrc is-live=true wave=sine freq=440 volume=0.5 name=micsrc");

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

}

#ifdef HAVE_LIGHTNING_WEBRTC
namespace {
struct SlowTestMic {
    GstBin parent;
};
struct SlowTestMicClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowTestMic, slow_test_mic, GST_TYPE_BIN)

constexpr int kSlowTestMicMs = 2500;

GstStateChangeReturn slowTestMicChangeState(GstElement *element,
                                            GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED)
        g_usleep(gulong(kSlowTestMicMs) * 1000);
    return GST_ELEMENT_CLASS(slow_test_mic_parent_class)
        ->change_state(element, transition);
}

void slow_test_mic_init(SlowTestMic *self)
{
    GstElement *device = gst_element_factory_make("audiotestsrc", nullptr);
    g_object_set(device, "is-live", TRUE, "volume", 0.05, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", inner));
    gst_object_unref(inner);
}

void slow_test_mic_class_init(SlowTestMicClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-start microphone (test)",
        "Source/Audio", "A capture whose sound server answers late",
        "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowTestMicChangeState;
}
}
#endif

class AudioDeviceTesterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
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
        QVERIFY(!tester.built());
        tester.setRuntimeAvailable(true);
        QVERIFY(!tester.available());
#else
        QVERIFY(tester.built());
#endif
    }

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
        QVERIFY(!built.contains(QStringLiteral("outsink")));

        QTRY_VERIFY_WITH_TIMEOUT(tester.level() > 0.1, 5000);

        levels.clear();
        QTest::qWait(1000);
        QVERIFY2(levels.count()
                     <= 1000 / lightning::calls::kMeterMinIntervalMs + 2,
                 qPrintable(QString::number(levels.count())));

        tester.stop();
        QCOMPARE(tester.mode(), AudioDeviceTester::Mode::Idle);
        QCOMPARE(tester.level(), 0.0);
        QVERIFY(!tester.pipelineRunningForTest());
        tester.noteLevelForTest(-3.0, 10'000'000);
        QCOMPARE(tester.level(), 0.0);
    }

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
        QVERIFY(!built.contains(QStringLiteral("micsrc")));
    }

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
        QVERIFY(tester.error().isEmpty());
    }

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
        QTest::qWait(300);
    }

    void aMicrophoneTestNeverWaitsOnASlowSoundServer()
    {
        if (!gstreamerWithMeter())
            QSKIP("GStreamer or its `level` element is absent");
        static const bool registered = gst_element_register(
            nullptr, "lightningslowtestmic", GST_RANK_NONE,
            slow_test_mic_get_type());
        QVERIFY(registered);
        AudioDeviceTester tester;
        tester.setTestSourceMode(true);
        tester.setRuntimeAvailable(true);
        tester.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowtestmic"));
        QElapsedTimer took;
        took.start();
        QVERIFY(tester.startMicrophoneTest(false));
        tester.stop();
        QVERIFY2(took.elapsed() < kSlowTestMicMs / 2,
                 qPrintable(QStringLiteral("starting and stopping waited %1 "
                                           "ms on the capture")
                                .arg(took.elapsed())));
        QTest::qWait(kSlowTestMicMs + 1000);
        QVERIFY(!tester.pipelineRunningForTest());
        QCOMPARE(tester.startsReportedForTest(), 0);
        tester.setMicrophoneSourceForTest(QString());
        QVERIFY(tester.startMicrophoneTest(false));
        QTRY_COMPARE_WITH_TIMEOUT(tester.startsReportedForTest(), 1, 8000);
        tester.stop();
    }

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
        const QString loud = AudioDeviceTester::describeForTest(
            AudioDeviceTester::Mode::Meter,
            QStringLiteral("autoaudiosrc name=micsrc"), 0, QString(), 200);
        QVERIFY2(loud.contains(QStringLiteral("volume=%1").arg(
                     QString::number(SfuMediaEngine::audioFactorPercent(200)
                                         / 100.0,
                                     'f', 3))),
                 qPrintable(loud));
    }

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
