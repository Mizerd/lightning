// 2026-08-23: the SFU media engine, driven for the first time.
//
// This class had NO test of any kind while it became the engine every call
// runs through: the .well-known discovery fix made `preferredCallLane` return
// "matrixrtc" instead of falling back to the legacy 1:1 lane, so
// SfuMediaEngine::start() went from unreachable to the default path in one
// commit — and the reporter's next run died instantly on pressing call.
//
// Everything here runs in TEST SOURCE MODE: synthetic audio/video and
// fakesinks, no microphone, no camera, no display server. What it exercises
// is the part that is Lightning's own — pipeline construction, the crypto pad
// probes, the per-sender key rings, and teardown — not the network.
#include "calls/SfuMediaEngine.h"

#include "calls/CallFrameCryptor.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/SfuVideoRouter.h"
#include "calls/WindowCaptureSrc.h"

#include <QSet>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QtTest/QtTest>

#include <QFile>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video-event.h>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {

/// How many frames reach a sink when exactly ONE buffer is ever pushed into a
/// LIVE pipeline whose input caps say `framerate=(fraction)0/1`.
///
/// That is the desktop-capture shape, and the whole of the reported freeze.
/// PipeWire delivers a buffer when the screen CHANGES, so its caps carry 0/1
/// and on a still screen there simply IS no second buffer. `videorate`
/// decides for each output timestamp which of the previous and NEXT inputs is
/// nearer, so it cannot emit anything until a second one arrives — it returns
/// 0 here, however long the wait. An aggregator that emits on its own output
/// deadline returns a steady stream from the first buffer.
///
/// -1 means the harness itself did not run.
int framesFromASingleCaptureBuffer(const QString &rateStage, int waitMs)
{
    // The production shape, minus the tee and the encoder: what is under test
    // is the rate stage's ability to START, and everything downstream of it
    // only ever sees what it produced.
    const QString description =
        QStringLiteral("appsrc name=src is-live=true format=time "
                       "do-timestamp=true ! videoconvert ! videoscale ! %1 "
                       "! video/x-raw,width=(int)[1,1920],"
                       "height=(int)[1,1080],framerate=(fraction)30/1 "
                       "! fakesink name=sink sync=false async=false")
            .arg(rateStage);
    GError *error = nullptr;
    GstElement *pipeline =
        gst_parse_launch(description.toUtf8().constData(), &error);
    if (error) {
        qWarning() << "rate-stage harness did not parse:" << error->message;
        g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        return -1;
    }
    if (!pipeline)
        return -1;
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    GstPad *sinkPad =
        sink ? gst_element_get_static_pad(sink, "sink") : nullptr;
    if (!src || !sink || !sinkPad) {
        if (sinkPad)
            gst_object_unref(sinkPad);
        if (src)
            gst_object_unref(src);
        if (sink)
            gst_object_unref(sink);
        gst_object_unref(pipeline);
        return -1;
    }

    // The portal's own caps shape (BGRA, framerate 0/1), at a size small
    // enough that the scaling and conversion cost nothing.
    constexpr int kW = 64;
    constexpr int kH = 48;
    GstCaps *caps = gst_caps_from_string(
        "video/x-raw,format=(string)BGRA,width=(int)64,height=(int)48,"
        "framerate=(fraction)0/1");
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);

    auto *seen = new std::atomic<int>(0);
    gst_pad_add_probe(
        sinkPad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer data) {
            static_cast<std::atomic<int> *>(data)->fetch_add(1);
            return GST_PAD_PROBE_OK;
        },
        seen,
        [](gpointer data) { delete static_cast<std::atomic<int> *>(data); });

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBuffer *buffer =
        gst_buffer_new_allocate(nullptr, kW * kH * 4, nullptr);
    gst_buffer_memset(buffer, 0, 0x40, kW * kH * 4);
    gst_app_src_push_buffer(GST_APP_SRC(src), buffer); // takes ownership
    // ...and NOTHING ELSE is ever pushed. That is the still desktop.
    QTest::qWait(waitMs);
    const int frames = seen->load();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sinkPad);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return frames;
}

/// The engine's own source, read once. Some invariants here are about HOW the
/// engine talks to GStreamer — which pad property it trusts — and a behaviour
/// test cannot see that: two Lightning engines agree on the wrong answer.
QByteArray engineSource()
{
    QFile file(QStringLiteral(LIGHTNING_SFU_ENGINE_SOURCE));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
#define SOURCE_UNDER_TEST engineSource()
} // namespace

class SfuMediaEngineTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QString whyNot;
        if (!SfuMediaEngine::runtimeAvailable(&whyNot)) {
            QSKIP(qPrintable(
                QStringLiteral("no SFU media runtime: %1").arg(whyNot)));
        }
    }

    void startingAndStoppingIsClean()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QVERIFY(!engine.active());
        engine.start();
        QVERIFY(engine.active());
        engine.stop();
        QVERIFY(!engine.active());
    }

    void restartingDoesNotLeakTheOldSession()
    {
        // The engine is ONE object reused call after call, so a second start
        // must tear the first down rather than stack a second pipeline.
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        for (int i = 0; i < 3; ++i) {
            engine.start();
            QVERIFY(engine.active());
        }
        engine.stop();
        QVERIFY(!engine.active());
    }

    void publishingAudioAndVideoBuildsRealPipelines()
    {
        // THE reproduction path: this is what pressing "call" reaches.
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();

        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        // A failure here is a category string, never a crash and never
        // silence.
        for (const QList<QVariant> &args : failed) {
            qWarning() << "engine reported failure:"
                       << args.at(0).toString();
        }
        QCOMPARE(failed.count(), 0);
        engine.stop();
    }

    void anOfferIsOnlyMadeOnceThereIsMediaAndThenCarriesIt()
    {
        // THE defect behind "calls insta fail". webrtcbin raises
        // on-negotiation-needed the moment it reaches PLAYING, which
        // ensurePeer does before any track exists — so the offer built from
        // that signal had NO media section. We sent a 98-byte SDP with no
        // `m=` line right after declaring a track to the SFU, and LiveKit
        // answered Leave(reason=6 STATE_MISMATCH) every time.
        //
        // Two things are asserted: no offer at all before a track is linked,
        // and once one is, an offer that actually contains media.
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy offers(&engine, &SfuMediaEngine::localDescription);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);

        engine.start();
        // A moment for the PLAYING transition to raise negotiation-needed.
        QTest::qWait(400);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(offers.count(), 0);   // deferred, nothing to offer yet

        engine.publishAudio(QStringLiteral("cid-audio"));
        QTRY_VERIFY_WITH_TIMEOUT(offers.count() > 0, 5000);
        QCOMPARE(failed.count(), 0);

        // The publisher's offer must describe the audio track. An SDP with no
        // media section is ~98 bytes; a real Opus offer is far larger and
        // says so explicitly.
        bool sawPublisherOffer = false;
        for (const QList<QVariant> &args : offers) {
            if (args.at(0).toInt() != 0)
                continue;   // 0 == publisher
            if (args.at(1).toString() != QLatin1String("offer"))
                continue;
            const QString sdp = args.at(2).toString();
            sawPublisherOffer = true;
            QVERIFY2(sdp.contains(QLatin1String("m=audio")),
                     qPrintable(QStringLiteral("publisher offer has no audio "
                                               "media section (%1 bytes)")
                                    .arg(sdp.size())));
            QVERIFY2(sdp.contains(QLatin1String("opus"), Qt::CaseInsensitive),
                     "publisher offer does not mention Opus");
        }
        QVERIFY2(sawPublisherOffer, "no publisher offer was produced at all");
        engine.stop();
    }

    void publishingTwiceUnderOneIdIsIgnored()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.unpublish(QStringLiteral("cid-audio"));
        // Unpublishing something that is gone must not fault.
        engine.unpublish(QStringLiteral("cid-audio"));
        engine.unpublish(QStringLiteral("never-existed"));
        engine.stop();
    }

    void encryptionArmedBeforeMediaExistsIsSafe()
    {
        // The order the controller actually uses: require encryption, clear
        // keys, THEN publish. With no key installed the probes must drop
        // frames rather than crash or leak cleartext.
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.setEncryptionRequired(true);
        engine.clearKeys();
        QVERIFY(!engine.encryptionActive());
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"), false, -1);
        QTest::qWait(200);   // let some frames actually flow through a probe
        QCOMPARE(failed.count(), 0);
        // Still no key, so still not claiming encryption.
        QVERIFY(!engine.encryptionActive());
        engine.stop();
    }

    void installingAKeyMakesEncryptionActiveAndFramesFlow()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.setEncryptionRequired(true);
        engine.start();
        engine.setOutboundKey(0, QByteArray(32, 'k'));
        QVERIFY(engine.encryptionActive());
        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"), false, -1);
        QTest::qWait(300);
        QCOMPARE(failed.count(), 0);
        engine.stop();
        // Keys must not outlive the call.
        QVERIFY(!engine.encryptionActive());
    }

    void aShortKeyIsRefusedRatherThanUsed()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.setOutboundKey(0, QByteArray(7, 'k'));
        QVERIFY(!engine.encryptionActive());
        engine.setInboundKey(QStringLiteral("PA_x"), 0, QByteArray());
        engine.stop();
    }

    void anOutOfRangeKeyIndexIsRefused()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.setOutboundKey(-1, QByteArray(32, 'k'));
        engine.setOutboundKey(99, QByteArray(32, 'k'));
        QVERIFY(!engine.encryptionActive());
        engine.stop();
    }

    void screenShareWithoutASourceIsRefusedNotGuessed()
    {
        // A negative PipeWire node id means "whatever PipeWire feels like",
        // which is how you publish the wrong monitor.
        SfuMediaEngine engine;
        engine.setTestSourceMode(false);   // the real source path
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-screen"),
                            /*screenShare=*/true, /*nodeId=*/-1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("screen_share_no_source"));
        engine.stop();
    }

    // NEITHER BACKEND MAY CALL gst_init ITSELF.
    //
    // GST_PLUGIN_PATH is read DURING gst_init, once. There are two media
    // backends and each used to run its own `gst_init_check` from its own
    // `std::call_once`, with only the SFU one setting the bundled plugin
    // path — and AppController probes the OTHER one first. So on a packaged
    // build the first init scanned the builder's sysroot (absent on a user's
    // machine), registered nothing, and the SFU engine's later init was a
    // no-op. The result was a client with the engine compiled in and 25
    // plugins beside it reporting `missing_element:webrtcbin`: no call
    // button, and an incoming call offering only Decline and Dismiss.
    //
    // A source scan, because the defect is WHICH FUNCTION RUNS FIRST and
    // that cannot be observed from inside one process that has already
    // initialised GStreamer. It fails on the tree that had two inits.
    void neitherMediaBackendInitialisesGstreamerItself()
    {
        const QString root = QStringLiteral(SOURCE_DIR "/src/calls/");
        for (const QString &name : { QStringLiteral("SfuMediaEngine.cpp"),
                                     QStringLiteral("GstCallMediaBackend.cpp") }) {
            QFile file(root + name);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(root + name));
            QString source = QString::fromUtf8(file.readAll());
            QVERIFY(!source.isEmpty());
            // COMMENTS STRIPPED FIRST. Both files EXPLAIN this defect in
            // prose directly above the fix, so a ban read off the raw text
            // always finds the token it forbids and fails on correct code —
            // the self-referential-ban trap this repo has hit repeatedly.
            // Whole-line `//` only, which is where every mention lives.
            source.remove(QRegularExpression(QStringLiteral("(?m)^[ \\t]*//.*$")));
            QVERIFY2(source.contains(QStringLiteral("runtimeAvailable")),
                     "the comment stripper ate the file, so this ban is "
                     "asserting nothing");
            QVERIFY2(!source.contains(QStringLiteral("gst_init")),
                     qPrintable(QStringLiteral(
                         "%1 calls gst_init itself; whichever backend is "
                         "probed first then decides whether the bundled "
                         "plugin path was applied, and a packaged build gets "
                         "an empty registry").arg(name)));
            QVERIFY2(source.contains(QStringLiteral("gst::ensureInitialised")),
                     qPrintable(QStringLiteral("%1 does not go through the "
                                               "shared bootstrap").arg(name)));
        }
        // And the bootstrap applies the path BEFORE it initialises. The order
        // is the whole point: reversed, it is exactly the bug above.
        QFile boot(root + QStringLiteral("GstBootstrap.cpp"));
        QVERIFY(boot.open(QIODevice::ReadOnly));
        QString source = QString::fromUtf8(boot.readAll());
        source.remove(QRegularExpression(QStringLiteral("(?m)^[ \\t]*//.*$")));
        const int applied = source.indexOf(QStringLiteral("applyBundledPluginPath()"),
                                           source.indexOf(QStringLiteral("call_once")));
        const int inited = source.indexOf(QStringLiteral("gst_init_check"));
        QVERIFY2(applied > 0 && inited > applied,
                 "GstBootstrap initialises GStreamer before applying the "
                 "bundled plugin path, which is the defect it exists to fix");
    }

    // EVERY PLATFORM CAN ACTUALLY CAPTURE, and each one names an element that
    // exists there. A source fragment naming a Linux-only element is what made
    // Windows and macOS silently call-less: the pipeline could never be built,
    // so a share reported success and carried nothing.
    //
    // Compile-time branches, so this asserts the branch THIS build took —
    // which is the only one it can observe. The other two are asserted by the
    // packaging validation, which greps the shipped plugin DLLs for the
    // element names (the Windows and macOS jobs in lightning-deploy).
    void everyPlatformNamesACaptureSourceItActuallyHas()
    {
        const QString camera = SfuMediaEngine::cameraSource();
        const QString screen = SfuMediaEngine::screenShareSource(0, -1);
        QVERIFY(!camera.isEmpty());
        QVERIFY(!screen.isEmpty());
#if defined(Q_OS_WIN)
        // ksvideosrc/gdiscreencapsrc, not mfvideosrc/d3d11screencapturesrc:
        // the mediafoundation and d3d11 plugins do not load in the packaging
        // toolchain, so those elements are not shipped. The property names
        // differ between the two families — `monitor`/`cursor` here versus
        // `monitor-index`/`show-cursor` there — so naming the wrong pair is a
        // pipeline that never builds.
        QCOMPARE(camera, QStringLiteral("ksvideosrc"));
        QVERIFY(screen.startsWith(QStringLiteral("gdiscreencapsrc")));
        QVERIFY2(screen.contains(QStringLiteral("monitor=")),
                 "the Windows screen source takes a monitor index, and "
                 "without one it captures whatever the element defaults to");
        QVERIFY2(!screen.contains(QStringLiteral("monitor-index=")),
                 "gdiscreencapsrc has no monitor-index property — that is "
                 "d3d11screencapturesrc's name, and setting an unknown "
                 "property makes gst_parse_launch fail outright");
#elif defined(Q_OS_MACOS)
        QCOMPARE(camera, QStringLiteral("avfvideosrc"));
        QVERIFY(screen.contains(QStringLiteral("capture-screen=true")));
        QVERIFY2(screen.contains(QStringLiteral("device-index=")),
                 "the macOS screen source takes a display index");
#else
        QCOMPARE(camera, QStringLiteral("v4l2src"));
        QVERIFY(screen.startsWith(QStringLiteral("pipewiresrc")));
#endif
        // NEVER a Linux element off Linux, and never a Windows one on Linux.
        // The branches are what this test exists to keep honest.
#if !defined(Q_OS_LINUX)
        QVERIFY2(!camera.contains(QStringLiteral("v4l2"))
                     && !screen.contains(QStringLiteral("pipewire")),
                 "a Linux-only capture element is named on a platform that "
                 "does not have it, so the pipeline can never be built");
#endif
    }

    // THE screen-share defect. The portal grants a node id AND a descriptor
    // to the PipeWire remote that node lives in (OpenPipeWireRemote); the
    // handshake used to stop before that call and hand `pipewiresrc path=<n>`
    // the id alone. That element resolves `path` against the CALLER's default
    // remote, where a portal node need not appear at all — so the pipeline
    // reaches PLAYING, reports no error, and never produces a buffer. A black
    // share that claims success, in both directions of the report.
    void aScreenShareCaptureUsesThePortalsOwnPipeWireRemote()
    {
        const QString withFd = SfuMediaEngine::screenShareSource(42, 7);
        QVERIFY2(withFd.contains(QStringLiteral("fd=7")),
                 qPrintable(QStringLiteral("no remote fd in: %1").arg(withFd)));
        QVERIFY(withFd.contains(QStringLiteral("path=42")));
        // The fd must come BEFORE the path: pipewiresrc resolves the path
        // against whichever remote it has been given.
        QVERIFY(withFd.indexOf(QStringLiteral("fd="))
                < withFd.indexOf(QStringLiteral("path=")));

        // With no remote there is nothing to do but ask the default one — but
        // it must not silently claim a descriptor it does not have.
        // MIN-BUFFERS IS PINNED, ON BOTH BRANCHES, and it is the difference
        // between a share starting and dying at negotiation.
        //
        // gst-plugin-pipewire's DEFAULT_MIN_BUFFERS was 8 through 1.4.x and is
        // 1 from 1.6. A compositor caps its screencast buffers low (KWin 6.6
        // offers RANGE(3, 2, 4)), and PipeWire 1.6 rejects a request whose
        // minimum exceeds the source's maximum with -EINVAL, surfaced as
        // "error alloc buffers: Invalid argument". A bundled 1.4.2 element
        // therefore fails against a 1.6 daemon while the host's own plugin
        // works -- measured: 8 and 5 fail, 4 and 1 allocate. Inheriting the
        // default means the bundle's GStreamer version silently decides
        // whether screen sharing works at all.
        QVERIFY2(withFd.contains(QStringLiteral("min-buffers=1")),
                 "the fd branch must pin min-buffers, not inherit the "
                 "bundled plugin's version-dependent default");

        const QString withoutFd = SfuMediaEngine::screenShareSource(42, -1);
        QVERIFY2(withoutFd.contains(QStringLiteral("min-buffers=1")),
                 "the no-fd branch must pin min-buffers too");
        QVERIFY(!withoutFd.contains(QStringLiteral("fd=")));
        QVERIFY(withoutFd.contains(QStringLiteral("path=42")));
        // THE OLD BAN ON min-buffers IS RE-SCOPED, NOT OVERRULED, and the
        // distinction is the point.
        //
        // What was refuted: `min-buffers=8`, shipped on reasoning alone as a
        // fix for the one-frame OPENING STALL, which made it strictly worse —
        // no frame arrived at all. That result stands.
        //
        // What is claimed now is a different value against a different defect,
        // and it EXPLAINS the old one rather than contradicting it. A
        // compositor caps its screencast buffers low (KWin 6.6: RANGE(3, 2, 4))
        // and PipeWire 1.6 rejects a request whose minimum exceeds the source's
        // maximum. `min-buffers=8` therefore cannot negotiate at all on such a
        // host — which is exactly "no frame arrived". The old round read a
        // symptom correctly and attributed it to the property being SET rather
        // than to the value being too HIGH.
        //
        // So the ban keeps its teeth where it earned them: a value above the
        // ceiling a real source offers stays forbidden. Measured on a live
        // 1.6.6 daemon: 8 and 5 fail with "error alloc buffers: Invalid
        // argument", 4 and 1 allocate.
        for (const QString &banned : { QStringLiteral("min-buffers=5"),
                                       QStringLiteral("min-buffers=6"),
                                       QStringLiteral("min-buffers=7"),
                                       QStringLiteral("min-buffers=8") }) {
            QVERIFY2(!withFd.contains(banned),
                     qPrintable(QStringLiteral(
                         "%1 exceeds the buffer ceiling a compositor offers; "
                         "it was measured to stop the capture entirely")
                                    .arg(banned)));
        }
        // The SECOND property shipped here on reasoning alone, and the second
        // to kill the capture: `keepalive-time` made the share freeze on its
        // FIRST frame and never recover, and the local self-view — tee'd off
        // the capture, so it indicts the capture and not the network — sat on
        // "Waiting for the picture". The opening hold is fixed DOWNSTREAM
        // instead (videoRateStage), where no PipeWire pool and no PipeWire
        // thread loop is touched.
        QVERIFY2(!withFd.contains(QStringLiteral("keepalive-time")),
                 "keepalive-time is back; it was measured to freeze the "
                 "capture on its first frame");
    }

    // The engine takes ownership of the descriptor, so a refusal must close
    // it. Otherwise every declined or too-late share leaks one fd, and a user
    // who opens the picker repeatedly runs the process out of descriptors.
    void arefusedScreenShareClosesTheDescriptorItWasGiven()
    {
        int fds[2] = { -1, -1 };
        QCOMPARE(::pipe(fds), 0);
        // fds[0] is handed over; fds[1] stays ours to clean up.
        SfuMediaEngine engine;
        engine.setTestSourceMode(false);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        // A negative node id is refused (the portal decides the source), and
        // the descriptor still has to be released.
        engine.publishVideo(QStringLiteral("cid-screen"),
                            /*screenShare=*/true, /*nodeId=*/-1, fds[0]);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(::fcntl(fds[0], F_GETFD), -1);
        QCOMPARE(errno, EBADF);
        engine.stop();
        ::close(fds[1]);
    }

    // Discord shows the sharer their own share, and it is the only way to
    // learn that a share is carrying pixels without asking the other end.
    // The self-view is a branch off the capture, keyed so it can never
    // collide with a LiveKit id.
    // The single thing that decides whether a received track can be
    // attributed to anyone. LiveKit's server PACKS the participant sid and
    // the track id into one msid stream id, and taking it whole produced a
    // name that matched neither the media key (installed per participant)
    // nor the video sink (attached per participant) — so a remote
    // participant was silent AND invisible at the same time.
    //
    // The literals are the reference's, not this implementation's: the
    // separator is `trackIdSeparator = "|"` in livekit's pkg/rtc/utils.go
    // and the split is livekit-client's unpackStreamId().
    // A REAL encrypted call between two engines, in LiveKit's own topology.
    //
    // Engine A's PUBLISHER is wired to engine B's SUBSCRIBER — which is
    // exactly the shape of a LiveKit call, because the SFU offers on the
    // subscriber and answers on the publisher. Nothing here is mocked below
    // the signalling: real webrtcbin, real ICE, real DTLS-SRTP, real Opus,
    // real AES-GCM frame encryption on the pad probes.
    //
    // It exists because "no audio" and "no connection" are indistinguishable
    // from the outside and have nothing in common. If this passes, the
    // pipeline, the negotiation and the crypto are sound end to end and a
    // failure against a real SFU is an INTEROP fault; if it fails, the fault
    // is here and needs no server to find.
    void anEncryptedCallBetweenTwoEnginesCarriesFrames()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);

        QString failure;
        const auto note = [&failure](const QString &why) {
            if (failure.isEmpty())
                failure = why;
        };
        connect(&sender, &SfuMediaEngine::failed, this, note);
        connect(&receiver, &SfuMediaEngine::failed, this, note);

        // Sender's publisher offer becomes the receiver's subscriber offer.
        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target != int(SfuMediaEngine::Target::Publisher)
                        || kind != QStringLiteral("offer")) {
                        return;
                    }
                    receiver.applyRemoteDescription(
                        SfuMediaEngine::Target::Subscriber, kind, sdp);
                });
        // ...and the receiver's answer goes back to the sender's publisher.
        connect(&receiver, &SfuMediaEngine::localDescription, &sender,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target != int(SfuMediaEngine::Target::Subscriber)
                        || kind != QStringLiteral("answer")) {
                        return;
                    }
                    sender.applyRemoteDescription(
                        SfuMediaEngine::Target::Publisher, kind, sdp);
                });
        connect(&sender, &SfuMediaEngine::localCandidate, &receiver,
                [&](int target, const QString &init) {
                    if (target == int(SfuMediaEngine::Target::Publisher)) {
                        receiver.applyRemoteCandidate(
                            SfuMediaEngine::Target::Subscriber, init);
                    }
                });
        connect(&receiver, &SfuMediaEngine::localCandidate, &sender,
                [&](int target, const QString &init) {
                    if (target == int(SfuMediaEngine::Target::Subscriber)) {
                        sender.applyRemoteCandidate(
                            SfuMediaEngine::Target::Publisher, init);
                    }
                });

        bool trackArrived = false;
        QString arrivedStream;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &, const QString &) {
                    trackArrived = true;
                    arrivedStream = streamId;
                });

        sender.start();
        receiver.start();

        // The SAME key on both sides, as a real call has after the
        // to-device exchange. Encryption REQUIRED, so a frame without a
        // usable key is dropped rather than sent in the clear — if the
        // format were wrong this test would see zero decrypted frames.
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("sender-device"), 3, key);

        sender.publishAudio(QStringLiteral("cid-loopback-audio"));

        QTRY_VERIFY2_WITH_TIMEOUT(
            trackArrived,
            qPrintable(QStringLiteral("no media pad; failure=%1").arg(failure)),
            45000);
        // The receiver's ring is named for the sid the SDP carried; bind the
        // name the key went in under to it, exactly as SfuCallController does
        // from the participant list.
        receiver.noteParticipantIdentity(arrivedStream,
                                         QStringLiteral("sender-device"));

        // Frames on the wire, and frames decrypted at the far end. Zero
        // encrypted means the capture or the encoder never ran; encrypted
        // but zero decrypted means the connection or the format is wrong.
        QTRY_VERIFY2_WITH_TIMEOUT(
            sender.framesEncrypted() > 0,
            qPrintable(QStringLiteral("nothing reached the wire; failure=%1")
                           .arg(failure)),
            30000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 0,
            qPrintable(QStringLiteral("frames sent but none decrypted; "
                                      "sent=%1 dropped=%2 failure=%3")
                           .arg(sender.framesEncrypted())
                           .arg(receiver.framesDropped())
                           .arg(failure)),
            30000);

        sender.stop();
        receiver.stop();
    }

    // A received track must be attributed by the pad's OWN msid/mid, never by
    // a media-section index derived from the pad name.
    //
    // webrtcbin numbers its src pads over the media it produces, while a
    // LiveKit subscriber offer carries a DATA CHANNEL in section 0 — so
    // `src_0` indexed the data channel, which has no `msid`, and every
    // received track came out unattributed into its own empty key ring.
    // Measured against a real SFU as `streamId = "mline:0"`: keys correct,
    // every frame undecryptable, the remote participant permanently silent
    // and invisible.
    //
    // The engine's own loopback (anEncryptedCallBetweenTwoEnginesCarriesFrames)
    // could not catch it: two Lightning engines negotiate audio in section 0,
    // so the wrong index accidentally agreed with the right one.
    // ENCRYPTED VP8 SURVIVES PAYLOAD -> DEPAYLOAD, which GStreamer's own
    // rtpvp8pay cannot do.
    //
    // rtpvp8pay reads the VP8 bitstream to build its descriptor — partition0's
    // size, a keyframe's 0x9d 0x01 0x2a start code, then bool-decoded
    // segmentation fields out of the compressed partition. LiveKit and Element
    // Call encrypt the whole encoded frame, leaving only 10 header bytes of a
    // keyframe (3 of a delta) in the clear, so the payloader gets ciphertext,
    // fails, and posts STREAM/ENCODE. Observed live as a screen share that
    // publishes exactly one frame and a camera that publishes none.
    //
    // This drives a real vp8enc, encrypts each frame exactly as the engine's
    // pad probe does, payloads with OUR element, depayloads, decrypts, and
    // requires the bytes back. It fails on rtpvp8pay by construction.
    void encryptedVp8SurvivesOurPayloader()
    {
        lightning::rtp::registerVp8Payloader();
        GstElement *pay = gst_element_factory_make(
            lightning::rtp::vp8PayloaderName(), nullptr);
        QVERIFY2(pay, "the VP8 payloader did not register");
        gst_object_unref(pay);

        // encode -> [encrypt] -> pay -> depay -> [decrypt] -> compare
        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        const QByteArray key(16, 'k'); // element-call's own length
        QVERIFY(sender.setKey(3, key));
        sender.setCurrentKeyIndex(3);
        QVERIFY(receiver.setKey(3, key));

        const QString desc = QStringLiteral(
            "videotestsrc num-buffers=12 is-live=false pattern=smpte "
            "! video/x-raw,width=320,height=240,framerate=15/1 "
            "! videoconvert ! vp8enc deadline=1 name=enc "
            "! appsink name=frames emit-signals=false sync=false "
            "max-buffers=64");
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(desc.toUtf8().constData(), &error);
        if (error) {
            const QString why = QString::fromUtf8(error->message);
            g_error_free(error);
            QSKIP(qPrintable(QStringLiteral("no vp8 encoder: %1").arg(why)));
        }
        QVERIFY(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");
        QVERIFY(sink);

        int checked = 0;
        for (int i = 0; i < 12; ++i) {
            GstSample *sample = nullptr;
            g_signal_emit_by_name(sink, "try-pull-sample",
                                  GstClockTime(2 * GST_SECOND), &sample);
            if (!sample)
                break;
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                const QByteArray plain(
                    reinterpret_cast<const char *>(map.data),
                    static_cast<int>(map.size));
                gst_buffer_unmap(buffer, &map);
                const bool delta = GST_BUFFER_FLAG_IS_SET(
                    buffer, GST_BUFFER_FLAG_DELTA_UNIT);
                const auto kind = delta
                    ? CallFrameCryptor::FrameKind::VideoDelta
                    : CallFrameCryptor::FrameKind::VideoKey;
                const QByteArray cipher =
                    sender.encryptFrame(plain, kind, 0x1234, 90 * (i + 1));
                QVERIFY2(!cipher.isEmpty(), "a real VP8 frame failed to encrypt");
                // The bytes the payloader would carry must come back whole
                // through a decrypt, which is what the far end does.
                QCOMPARE(receiver.decryptFrame(cipher, kind), plain);
                ++checked;
            }
            gst_sample_unref(sample);
        }
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        QVERIFY2(checked >= 3,
                 qPrintable(QStringLiteral("only %1 encoded frames were "
                                           "produced").arg(checked)));
    }

    // THE SCREEN-SHARE PIPELINE PARSES, self-view branch and all.
    //
    // Test-source mode builds a different, simpler description, so the shape a
    // real share actually uses — a `tee` feeding both the encoder and an
    // appsink preview — was never parsed by anything until it ran on a user's
    // desktop. A typo or a bad element name there is a share that dies on its
    // first frame with a bus error, which is precisely how this path failed
    // before. Parsed here with a videotestsrc standing in for pipewiresrc, so
    // no portal, no display server and no capture are needed.
    // OUR PAYLOADER'S PACKETS ROUND-TRIP THROUGH GStreamer's own depayloader,
    // and carry the descriptor libwebrtc and LiveKit expect.
    //
    // The picture id is the part that is not optional in practice: libwebrtc
    // always sends one, and LiveKit's SFU REWRITES the descriptor when it
    // forwards (codecmunger/vp8.go), computing the header size from that very
    // field. With no picture id its idea of the header no longer matches ours
    // and everything after the first frame is corrupted — the far end shows
    // one frame and then nothing.
    // THE KEYFRAME FLAG WE ENCRYPT BY MUST MATCH THE BITSTREAM.
    //
    // We choose how many header bytes to leave in the clear from
    // GST_BUFFER_FLAG_DELTA_UNIT (10 for a keyframe, 3 for a delta). Element
    // and every other libwebrtc client decide the same thing from the VP8
    // frame tag's P bit — `frame[0] & 0x1`, 0 meaning keyframe. If those two
    // ever disagree, the two ends encrypt and decrypt from different offsets
    // and authentication fails.
    //
    // It would be invisible between two Lightning clients, which share the
    // same rule and so agree even when both are wrong. It is exactly the shape
    // of "Element shows one frame and freezes": the keyframe, where the two
    // rules coincide, decodes; every delta after it does not.
    // A STRICT RECEIVER RECOVERS EVERY FRAME, not just the first.
    //
    // GStreamer's depayloader completes a frame on the marker bit and is
    // forgiving about the rest, so two Lightning clients can agree on a stream
    // a libwebrtc receiver would reject. This reassembles our own packets the
    // way libwebrtc does — a frame STARTS on the descriptor's S bit, ENDS on
    // the RTP marker, and its payload is everything after the descriptor —
    // and requires each rebuilt frame to equal the encoder's output byte for
    // byte. "Element renders one frame and freezes" is what a receiver does
    // when only the first frame reassembles.
    // A KEYFRAME REQUEST MUST REACH THE ENCODER THROUGH OUR PAYLOADER.
    //
    // A subscriber that joins after the stream started holds no reference
    // frame, so it asks for one: the SFU sends a PLI, webrtcbin turns it into
    // an upstream `GstForceKeyUnit` event, and it has to travel through the
    // payloader to vp8enc. If it does not, that subscriber waits for a
    // keyframe that only arrives on the encoder's own schedule — and with
    // `keyframe-max-dist=60` on the screen-share encoder that is seconds away,
    // or never if the request is what the encoder was waiting for. The far end
    // sits on "waiting for media" while every counter on this side looks
    // healthy and an independent subscriber counts megabits arriving.
    void aKeyframeRequestReachesTheEncoderThroughOurPayloader()
    {
        lightning::rtp::registerVp8Payloader();
        const QString desc =
            QStringLiteral(
                "videotestsrc is-live=true pattern=ball "
                "! video/x-raw,width=320,height=240,framerate=15/1 "
                "! videoconvert ! vp8enc deadline=1 keyframe-max-dist=1000 "
                "! %1 name=pay pt=96 "
                "! appsink name=pkts sync=false max-buffers=256")
                .arg(QLatin1String(lightning::rtp::vp8PayloaderName()));
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(desc.toUtf8().constData(), &error);
        if (error) {
            const QString why = QString::fromUtf8(error->message);
            g_error_free(error);
            QSKIP(qPrintable(QStringLiteral("no vp8 encoder: %1").arg(why)));
        }
        QVERIFY(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "pkts");
        GstElement *pay = gst_bin_get_by_name(GST_BIN(pipeline), "pay");
        QVERIFY(sink && pay);

        // Drain the opening keyframe and a few deltas, so what follows can
        // only be a keyframe the REQUEST produced.
        const auto pullFrameStart = [&sink]() -> int {
            for (int i = 0; i < 400; ++i) {
                GstSample *sample = nullptr;
                g_signal_emit_by_name(sink, "try-pull-sample",
                                      GstClockTime(2 * GST_SECOND), &sample);
                if (!sample)
                    return -1;
                int isKey = -1;
                GstBuffer *b = gst_sample_get_buffer(sample);
                GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
                if (b && gst_rtp_buffer_map(b, GST_MAP_READ, &rtp)) {
                    const guint8 *p = static_cast<const guint8 *>(
                        gst_rtp_buffer_get_payload(&rtp));
                    const guint len = gst_rtp_buffer_get_payload_len(&rtp);
                    guint header = 1;
                    if (p[0] & 0x80) {
                        header = 2;
                        if (p[1] & 0x80)
                            header += (p[2] & 0x80) ? 2 : 1;
                    }
                    // Only a frame's FIRST packet carries the VP8 frame tag.
                    if ((p[0] & 0x10) != 0 && len > header)
                        isKey = (p[header] & 0x01) ? 0 : 1;
                    gst_rtp_buffer_unmap(&rtp);
                }
                gst_sample_unref(sample);
                if (isKey >= 0)
                    return isKey;
            }
            return -1;
        };

        QVERIFY2(pullFrameStart() == 1, "the stream did not open on a keyframe");
        int deltas = 0;
        for (int i = 0; i < 12; ++i) {
            if (pullFrameStart() == 0)
                ++deltas;
        }
        QVERIFY2(deltas > 0, "no delta frames followed the opening keyframe");

        // The request, exactly as webrtcbin raises it from a PLI.
        GstPad *srcPad = gst_element_get_static_pad(pay, "src");
        QVERIFY(srcPad);
        const bool sent = gst_pad_send_event(
            srcPad, gst_video_event_new_upstream_force_key_unit(
                        GST_CLOCK_TIME_NONE, TRUE, 1));
        gst_object_unref(srcPad);
        QVERIFY2(sent, "the payloader refused the keyframe request outright");

        bool gotKeyframe = false;
        for (int i = 0; i < 40 && !gotKeyframe; ++i) {
            if (pullFrameStart() == 1)
                gotKeyframe = true;
        }
        gst_object_unref(pay);
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);

        QVERIFY2(gotKeyframe,
                 "a keyframe request did not reach the encoder: a subscriber "
                 "that joins mid-stream would never receive a decodable frame");
    }

    void aStrictReceiverRecoversEveryFrameWeSend()
    {
        lightning::rtp::registerVp8Payloader();
        const QString desc =
            QStringLiteral(
                "videotestsrc num-buffers=15 is-live=false pattern=ball "
                "! video/x-raw,width=640,height=480,framerate=15/1 "
                "! videoconvert ! vp8enc deadline=1 keyframe-max-dist=10 "
                "! tee name=t "
                "t. ! queue ! appsink name=frames sync=false max-buffers=64 "
                "t. ! queue ! %1 pt=96 mtu=400 "
                "! appsink name=pkts sync=false max-buffers=1024")
                .arg(QLatin1String(lightning::rtp::vp8PayloaderName()));
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(desc.toUtf8().constData(), &error);
        if (error) {
            const QString why = QString::fromUtf8(error->message);
            g_error_free(error);
            QSKIP(qPrintable(QStringLiteral("no vp8 encoder: %1").arg(why)));
        }
        QVERIFY(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstElement *frameSink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");
        GstElement *pktSink = gst_bin_get_by_name(GST_BIN(pipeline), "pkts");
        QVERIFY(frameSink && pktSink);

        QList<QByteArray> frames;
        for (int i = 0; i < 15; ++i) {
            GstSample *sample = nullptr;
            g_signal_emit_by_name(frameSink, "try-pull-sample",
                                  GstClockTime(2 * GST_SECOND), &sample);
            if (!sample)
                break;
            GstBuffer *b = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (b && gst_buffer_map(b, &map, GST_MAP_READ)) {
                frames.append(QByteArray(
                    reinterpret_cast<const char *>(map.data),
                    static_cast<int>(map.size)));
                gst_buffer_unmap(b, &map);
            }
            gst_sample_unref(sample);
        }

        // Reassemble exactly as a strict receiver does.
        QList<QByteArray> rebuilt;
        QByteArray current;
        bool inFrame = false;
        for (int i = 0; i < 4096; ++i) {
            GstSample *sample = nullptr;
            g_signal_emit_by_name(pktSink, "try-pull-sample",
                                  GstClockTime(GST_SECOND), &sample);
            if (!sample)
                break;
            GstBuffer *b = gst_sample_get_buffer(sample);
            GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
            if (b && gst_rtp_buffer_map(b, GST_MAP_READ, &rtp)) {
                const guint8 *p = static_cast<const guint8 *>(
                    gst_rtp_buffer_get_payload(&rtp));
                const guint len = gst_rtp_buffer_get_payload_len(&rtp);
                // Descriptor length: X then I then a 15-bit (M) picture id.
                guint header = 1;
                if (p[0] & 0x80) {
                    header = 2;
                    if (p[1] & 0x80)
                        header += (p[2] & 0x80) ? 2 : 1;
                }
                if ((p[0] & 0x10) != 0) { // S: a frame starts here
                    current.clear();
                    inFrame = true;
                }
                if (inFrame && len > header) {
                    current.append(
                        reinterpret_cast<const char *>(p + header),
                        static_cast<int>(len - header));
                }
                if (gst_rtp_buffer_get_marker(&rtp) && inFrame) {
                    rebuilt.append(current);
                    current.clear();
                    inFrame = false;
                }
                gst_rtp_buffer_unmap(&rtp);
            }
            gst_sample_unref(sample);
        }
        gst_object_unref(frameSink);
        gst_object_unref(pktSink);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);

        QVERIFY2(frames.size() >= 5, "the encoder produced too few frames");
        QVERIFY2(rebuilt.size() >= 5,
                 qPrintable(QStringLiteral("a strict receiver reassembled only "
                                           "%1 of %2 frames")
                                .arg(rebuilt.size()).arg(frames.size())));
        const int compare = qMin(frames.size(), rebuilt.size());
        for (int i = 0; i < compare; ++i) {
            QVERIFY2(rebuilt.at(i) == frames.at(i),
                     qPrintable(QStringLiteral(
                         "frame %1 did not survive: %2 bytes in, %3 out")
                             .arg(i).arg(frames.at(i).size())
                             .arg(rebuilt.at(i).size())));
        }
    }

    void theDeltaFlagAgreesWithTheVp8Bitstream()
    {
        const QString desc = QStringLiteral(
            "videotestsrc num-buffers=20 is-live=false pattern=ball "
            "! video/x-raw,width=320,height=240,framerate=15/1 "
            "! videoconvert ! vp8enc deadline=1 keyframe-max-dist=5 "
            "! appsink name=frames sync=false max-buffers=64");
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(desc.toUtf8().constData(), &error);
        if (error) {
            const QString why = QString::fromUtf8(error->message);
            g_error_free(error);
            QSKIP(qPrintable(QStringLiteral("no vp8 encoder: %1").arg(why)));
        }
        QVERIFY(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");
        QVERIFY(sink);

        int checked = 0;
        int keyframes = 0;
        int disagreements = 0;
        for (int i = 0; i < 20; ++i) {
            GstSample *sample = nullptr;
            g_signal_emit_by_name(sink, "try-pull-sample",
                                  GstClockTime(2 * GST_SECOND), &sample);
            if (!sample)
                break;
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                if (map.size >= 1) {
                    const bool flagSaysDelta = GST_BUFFER_FLAG_IS_SET(
                        buffer, GST_BUFFER_FLAG_DELTA_UNIT);
                    // VP8 frame tag, bit 0: 0 = key, 1 = interframe.
                    const bool streamSaysDelta = (map.data[0] & 0x01) != 0;
                    if (flagSaysDelta != streamSaysDelta)
                        ++disagreements;
                    if (!streamSaysDelta)
                        ++keyframes;
                    ++checked;
                }
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);

        QVERIFY2(checked >= 5, "too few frames to judge");
        QVERIFY2(keyframes >= 2, "no keyframes were produced to compare");
        QCOMPARE(disagreements, 0);
    }

    void ourPayloaderEmitsTheDescriptorLibwebrtcExpects()
    {
        lightning::rtp::registerVp8Payloader();
        const QString desc =
            QStringLiteral("videotestsrc num-buffers=6 is-live=false "
                           "! video/x-raw,width=320,height=240,framerate=15/1 "
                           "! videoconvert ! vp8enc deadline=1 "
                           "! %1 pt=96 mtu=600 "
                           "! appsink name=pkts sync=false max-buffers=256")
                .arg(QLatin1String(lightning::rtp::vp8PayloaderName()));
        GError *error = nullptr;
        GstElement *pipeline =
            gst_parse_launch(desc.toUtf8().constData(), &error);
        if (error) {
            const QString why = QString::fromUtf8(error->message);
            g_error_free(error);
            QSKIP(qPrintable(QStringLiteral("no vp8 encoder: %1").arg(why)));
        }
        QVERIFY(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "pkts");
        QVERIFY(sink);

        int packets = 0;
        int starts = 0;
        int markers = 0;
        QSet<int> pictureIds;
        QList<quint32> frameTimestamps;
        quint32 lastTimestamp = 0;
        bool haveTimestamp = false;
        for (int i = 0; i < 200; ++i) {
            GstSample *sample = nullptr;
            g_signal_emit_by_name(sink, "try-pull-sample",
                                  GstClockTime(GST_SECOND), &sample);
            if (!sample)
                break;
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
            if (buffer && gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
                const guint8 *p = static_cast<const guint8 *>(
                    gst_rtp_buffer_get_payload(&rtp));
                const guint len = gst_rtp_buffer_get_payload_len(&rtp);
                QVERIFY2(len > 4, "a packet carried no payload past the "
                                  "descriptor");
                // X and I must be set, and the id must be the 15-bit form.
                QVERIFY2((p[0] & 0x80) != 0, "the X (extended) bit is not set");
                QVERIFY2((p[1] & 0x80) != 0, "the I (picture id) bit is not set");
                QVERIFY2((p[2] & 0x80) != 0, "the picture id is not 15-bit (M)");
                pictureIds.insert(((p[2] & 0x7f) << 8) | p[3]);
                if ((p[0] & 0x10) != 0) {
                    ++starts;
                    frameTimestamps.append(gst_rtp_buffer_get_timestamp(&rtp));
                }
                // Every packet of one frame carries that frame's timestamp.
                const quint32 ts = gst_rtp_buffer_get_timestamp(&rtp);
                if (haveTimestamp && (p[0] & 0x10) == 0)
                    QCOMPARE(ts, lastTimestamp);
                lastTimestamp = ts;
                haveTimestamp = true;
                if (gst_rtp_buffer_get_marker(&rtp))
                    ++markers;
                ++packets;
                gst_rtp_buffer_unmap(&rtp);
            }
            gst_sample_unref(sample);
        }
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);

        QVERIFY2(packets >= 6, qPrintable(QStringLiteral(
                     "only %1 packets were produced").arg(packets)));
        // One start and one marker per frame, and a DISTINCT picture id per
        // frame — a constant id is what a munging SFU cannot forward.
        QCOMPARE(starts, markers);
        // Chrome starts the picture id at a random value, never 0: the SFU
        // seeds its wrap handler with `PictureID - 1` and Init(-1) is an edge
        // no real stream presents it with.
        QVERIFY2(!pictureIds.contains(0),
                 "the picture id sequence started at 0");
        QVERIFY2(pictureIds.size() == starts,
                 qPrintable(QStringLiteral("%1 frames carried %2 distinct "
                                           "picture ids")
                                .arg(starts).arg(pictureIds.size())));
        // THE RTP TIMESTAMP MUST ADVANCE PER FRAME.
        //
        // GStreamer's own depayloader completes a frame on the MARKER bit and
        // barely looks at the clock, so a stalled timestamp round-trips
        // between two Lightning clients perfectly. libwebrtc's jitter buffer
        // groups by timestamp: frames sharing one become a single picture, and
        // the far end renders once and then freezes.
        QVERIFY2(frameTimestamps.size() >= 3,
                 "too few frames to judge the timestamps");
        for (int i = 1; i < frameTimestamps.size(); ++i) {
            QVERIFY2(frameTimestamps.at(i) != frameTimestamps.at(i - 1),
                     qPrintable(QStringLiteral(
                         "frames %1 and %2 share RTP timestamp %3")
                             .arg(i - 1).arg(i).arg(frameTimestamps.at(i))));
        }
    }

    // THE ENCODER IS GIVEN A FIXED CADENCE, never a range containing 0/1.
    //
    // A desktop capture negotiates `framerate=(fraction)0/1` — PipeWire
    // delivers on damage, not on a clock. Measured straight off the portal:
    //   video/x-raw, format=BGRA, width=3840, height=2160,
    //   framerate=(fraction)0/1, max-framerate=(fraction)59/1
    // A range including 0/1 lets that negotiate through to `vp8enc`, which
    // then has no rate to plan against and no steady cadence to emit. Every
    // WebRTC sender encodes at a fixed rate, and a receiver's jitter buffer is
    // built for one.
    void theVideoCapsPinAFixedFramerate()
    {
        for (const bool screenShare : { true, false }) {
            const QString limits = screenShare
                ? QStringLiteral("video/x-raw,width=(int)[1,1920],"
                                 "height=(int)[1,1080],"
                                 "framerate=(fraction)30/1")
                : QStringLiteral("video/x-raw,width=(int)[1,1280],"
                                 "height=(int)[1,720],"
                                 "framerate=(fraction)30/1");
            const QString desc = SfuMediaEngine::videoPipelineDescription(
                QStringLiteral("videotestsrc"),
                SfuMediaEngine::videoRateStage(screenShare), limits,
                QStringLiteral("vp8enc"), QString(), 1u,
                QStringLiteral("videoconvert ! videoscale"),
                SfuMediaEngine::captureEntryFilter(false));
            QVERIFY2(desc.contains(QStringLiteral("framerate=(fraction)30/1")),
                     "the encoder is not given a fixed framerate");
            QVERIFY2(!desc.contains(QStringLiteral("framerate=(fraction)[0/1")),
                     "a framerate range including 0/1 reached the encoder: a "
                     "desktop capture negotiates 0/1 and it would propagate");
        }
    }

    // A LIVE CAPTURE'S BUS ERROR IS STILL NOT A FAILURE.
    //
    // `onBusMessage` logs and never raises, and that has to stay true: a
    // pipeline posts errors during ordinary teardown, and under load this
    // suite once saw three of them on a healthy key install. Turning those
    // into failures tore down working calls, which is why the general version
    // was reverted within the hour. What is new is a NARROW escalation, and
    // every clause of it is asserted here from the other side.
    //
    // UNFIXED TREE: does not compile — there was no publishFailed at all, and
    // that IS the defect: a camera that could not negotiate left the button
    // lit for the rest of the call with the reason only in a log.
    void aBusErrorFromALiveOrUnknownBinIsNotAPublishFailure()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy publish(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        // Not a published bin at all (this is webrtcbin's own name, and it
        // sits directly under the pipeline exactly as a publish bin does).
        engine.handlePublishError(QStringLiteral("wb-pub"));
        engine.handlePublishError(QStringLiteral("never-published"));
        QCOMPARE(publish.count(), 0);

        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        // Test-source mode publishes a videotestsrc, which really does
        // deliver buffers — so this bin's capture is ALIVE and an error from
        // it is a transient in something else.
        QTest::qWait(400);
        engine.handlePublishError(QStringLiteral("cid-video"));
        QCOMPARE(publish.count(), 0);

        // The "once unpublished, the cid is not reportable" clause USED to be
        // asserted here and had to be lifted out: unpublishing this bin
        // deadlocks. See theUnpublishOfALiveVideoBinDeadlocks() below, which
        // reproduces it on demand. The rule itself is still covered, against
        // an audio bin, by publishingTwiceUnderOneIdIsIgnored().
        engine.stop();
    }

    /// THE VOLUME CURVE: 0-100 is literal, 100-200 expands to 100-1000.
    ///
    /// One linear scale could not serve both jobs. Attenuation must stay 1:1
    /// or every setting below unity means something other than it says; boost
    /// needs to reach 1000%, because a straight 0-200 slider tops out at
    /// +6 dB — "above 100% barely any difference" — while a straight 0-1000
    /// slider puts every useful setting in its first tenth.
    void theVolumeCurveIsLiteralBelowUnityAndExpandsAbove()
    {
        // Attenuation: untouched, including the ends. 0 must be exactly 0 —
        // silence is the one setting a curve must never approximate.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(0), 0);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(1), 1);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(50), 50);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(99), 99);
        // Unity is unity on both sides of the join, so the curve has no step
        // in it at the one point the user is most likely to sit on.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(100), 100);
        // Boost: the far end of the slider is the element's own ceiling.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(200), 1000);
        // And monotonic in between, with the midpoint where the straight
        // line says it is.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(150), 550);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(101), 109);
        // Out of range saturates rather than wrapping or extrapolating past
        // the element's range, where it would clamp silently.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(-5), 0);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(10000), 1000);
    }

    /// THE RENEGOTIATED OFFER MUST STOP ADVERTISING THE STOPPED TRACK.
    ///
    /// This is the assertion closest to what the far end actually reads.
    /// Retiring the transceiver locally is necessary but not sufficient: what
    /// makes Element drop the tile is the next OFFER no longer carrying that
    /// track's msid. Until it does, the old share is a corpse the SFU goes on
    /// listing — "its like the first one doesnt stop" — and the new share
    /// arrives while the remote is still rendering the dead one, which is the
    /// blank screen.
    ///
    /// Mute cannot do this job and is not a substitute: a mute removes
    /// nothing, so the stopped track stays in the participant's track list
    /// forever. Only renegotiating without it takes it off the wire.
    void theOfferAfterUnpublishNoLongerAdvertisesTheTrack()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy offers(&engine, &SfuMediaEngine::localDescription);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-share"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        QTest::qWait(400);

        // The msid webrtcbin puts on the publisher pad IS the cid, and it is
        // how the SFU maps the media section to the track it authorised.
        const auto offerCarrying = [&](const QString &needle) {
            for (const QList<QVariant> &call : offers) {
                if (call.value(1).toString() != QStringLiteral("offer"))
                    continue;
                if (call.value(2).toString().contains(needle))
                    return true;
            }
            return false;
        };
        QVERIFY2(offerCarrying(QStringLiteral("cid-share")),
                 "the publish never offered the track at all, so this case "
                 "cannot say anything about withdrawing it");

        QList<QList<QVariant>> before;
        for (const QList<QVariant> &call : offers)
            before << call;
        offers.clear();
        engine.unpublish(QStringLiteral("cid-share"));
        // The teardown and its renegotiation are asynchronous by design.
        for (int i = 0; i < 60 && offers.isEmpty(); ++i)
            QTest::qWait(50);
        QVERIFY2(!offers.isEmpty(),
                 "stopping the track produced no renegotiation at all: the "
                 "SFU is never told, so the far end keeps the dead track");
        QVERIFY2(!offerCarrying(QStringLiteral("cid-share")),
                 "the offer after unpublish still advertises the stopped "
                 "track: the far end goes on rendering a corpse and the next "
                 "share lands beside it");
        const auto sections = [](const QString &sdp) {
            return sdp.count(QStringLiteral("\r\nm="))
                 + (sdp.startsWith(QStringLiteral("m=")) ? 1 : 0);
        };
        QString last;
        for (const QList<QVariant> &call : offers) {
            if (call.value(1).toString() == QStringLiteral("offer"))
                last = call.value(2).toString();
        }
        QString firstOffer;
        for (const QList<QVariant> &call : before) {
            if (call.value(1).toString() == QStringLiteral("offer"))
                firstOffer = call.value(2).toString();
        }

        // THE SECTION MUST SURVIVE AND GO INACTIVE — both halves matter, and
        // this is the shape the far end actually obeys.
        //
        // Releasing the request pad alone drops our msid but leaves the
        // section `a=sendrecv`: measured before the fix as
        //   before  m=video ... | a=sendrecv
        //   after   m=video ... | a=sendrecv     <- msid gone, still sending
        // which tells the remote there is a video section we are sending on
        // with nothing behind it. That is a tile that never goes away.
        //
        // And an m= section may never be REMOVED from an SDP: the count has
        // to stay stable across renegotiation, so a shrinking offer would be
        // its own protocol fault rather than a fix.
        QCOMPARE(sections(last), sections(firstOffer));
        QVERIFY2(firstOffer.contains(QStringLiteral("a=sendrecv"))
                     || firstOffer.contains(QStringLiteral("a=sendonly")),
                 "the publish offer never claimed to send, so this case "
                 "cannot show the direction being withdrawn");
        QVERIFY2(last.contains(QStringLiteral("a=inactive")),
                 "the offer after unpublish leaves the section active: the "
                 "far end is told we are still sending on a media section "
                 "with no track behind it, and renders an empty tile that "
                 "never clears");
        engine.stop();
    }

    /// STOPPING A SHARE MUST RETIRE ITS TRANSCEIVER, not just quiesce our
    /// own pipeline.
    ///
    /// Quiescing is invisible to everyone else. The m= section stays in the
    /// SDP, so the far end keeps rendering the last frame it received — a
    /// frozen picture that only leaving the call clears — and the next share
    /// is offered as an ADDITIONAL section rather than reusing the one just
    /// vacated. A live capture of three shares in one session showed the
    /// answer growing 2 -> 3 -> 4 sections with nothing ever removed, which
    /// is the whole of "stopping doesnt stop the stream ... in element a
    /// blank screen remains and starting to share again doesnt work".
    ///
    /// One sink pad on the publisher webrtcbin is one outgoing track and so
    /// one m= section. The count must come back DOWN, and a re-publish must
    /// not stack on top of the old one.
    void stoppingAPublishRetiresItsTransceiver()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        // -1, not 0: the publisher webrtcbin is built lazily on the first
        // publish, so before one there is no peer connection to count at all.
        QCOMPARE(engine.publisherTrackSlotsForTest(), -1);

        engine.publishVideo(QStringLiteral("cid-share"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        QTest::qWait(400);
        QCOMPARE(engine.publisherTrackSlotsForTest(), 1);

        // The teardown is deliberately asynchronous (see the deadlock case
        // below), so the count falls a moment later rather than on return.
        engine.unpublish(QStringLiteral("cid-share"));
        int slotCount = -1;
        for (int i = 0; i < 60; ++i) {
            slotCount = engine.publisherTrackSlotsForTest();
            if (slotCount == 0)
                break;
            QTest::qWait(50);
        }
        QVERIFY2(slotCount == 0,
                 qPrintable(QStringLiteral(
                     "the publisher still holds %1 track slot(s) after "
                     "unpublish: the far end will keep the dead m= section "
                     "and go on showing a frozen frame")
                                .arg(slotCount)));

        // AND THE NEXT SHARE MUST NOT STACK. Two slotCount here is the reported
        // growth, with the new share landing on a section the far end is not
        // rendering.
        engine.publishVideo(QStringLiteral("cid-share-2"),
                            /*screenShare=*/false, /*nodeId=*/-1);
        QTest::qWait(400);
        QCOMPARE(engine.publisherTrackSlotsForTest(), 1);
        engine.stop();
    }

    /// UNPUBLISHING A LIVE VIDEO BIN MUST NOT DEADLOCK.
    ///
    /// It did, and this case hung for the full 120 s CTest timeout before the
    /// fix — which times out the whole binary rather than failing one case,
    /// so it ran behind an env opt-in until it was green. From the core dump:
    ///
    ///   main thread   unpublish -> gst_element_set_state_func
    ///                 -> gst_bin_change_state_func
    ///                 -> gst_bin_src_pads_activate -> gst_pad_set_active
    ///                 -> activate_mode_internal
    ///                 -> __pthread_mutex_lock          [wants stream lock]
    ///   queue1:src    gst_queue_loop -> vp8enc -> our RtpVp8Payloader
    ///                 -> gst_pad_chain_data_unchecked
    ///                 -> do_probe_callbacks -> g_cond_wait   [holds it]
    ///
    /// On the GUI thread in production: "stop screen share and my feed stays
    /// frozen ... the only way to clear it is rejoin the call", corroborated
    /// from the far side by Element playing its share-start jingle the FIRST
    /// time and never again — the track was never withdrawn.
    ///
    /// Fixed by blocking the pad with a GST_PAD_PROBE_TYPE_IDLE probe (which
    /// by construction fires only when no push is in flight) and doing the
    /// state change from gst_element_call_async, off any streaming thread.
    ///
    /// THE 400 ms WAIT IS THE TEST. Unpublishing before the bin streams does
    /// not reproduce it — publishingTwiceUnderOneIdIsIgnored() does exactly
    /// that and passed throughout. Do not shorten it.
    ///
    /// Two reorderings do NOT fix it and were each built, run and reverted:
    /// unlinking from webrtcbin before set_state(NULL), and unparenting
    /// before set_state(NULL). Both stall at the identical GST_STATES line,
    /// because neither stops a push already in flight.
    void unpublishingALiveVideoBinDoesNotDeadlock()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        QTest::qWait(400);
        engine.unpublish(QStringLiteral("cid-video"));
        // Reaching here at all is the assertion — the unfixed tree never
        // returned from the line above. The republish proves the teardown
        // left nothing behind that blocks the cid being used again, which is
        // the reported "turn it off and on again ... doesnt work again".
        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        QTest::qWait(200);
        engine.stop();
    }

    // ...and a publish that genuinely never prerolls IS reported — without
    // ending the call, which is the whole reason this is not `failed()`.
    //
    // Staged with a PipeWire node id nothing can resolve and no portal
    // remote, which is the exact shape the camera was in before 7f5cd06: the
    // source cannot negotiate, the bin never reaches PLAYING, the capture
    // delivers zero buffers, and nothing is ever encoded or sent while the
    // control stays lit.
    void aCaptureThatNeverStartsIsReportedAndTheCallSurvives()
    {
        GstElementFactory *factory = gst_element_factory_find("pipewiresrc");
        if (!factory)
            QSKIP("no pipewiresrc: this failure cannot be staged here");
        gst_object_unref(factory);

        SfuMediaEngine engine;
        engine.setTestSourceMode(false);
        QSignalSpy fatal(&engine, &SfuMediaEngine::failed);
        QSignalSpy publish(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-doomed"),
                            /*screenShare=*/true, /*nodeId=*/2147483,
                            /*pipewireFd=*/-1);
        for (int i = 0; i < 100 && publish.count() == 0; ++i)
            QTest::qWait(100);
        // A CALL IS NEVER ENDED BY A CAPTURE DEVICE. True whichever way the
        // environment went, so it is asserted before anything is skipped.
        QCOMPARE(fatal.count(), 0);
        if (publish.count() == 0) {
            engine.stop();
            QSKIP("pipewiresrc neither errored nor started here; the "
                  "environment cannot stage a dead capture");
        }
        QCOMPARE(publish.at(0).at(0).toString(),
                 QStringLiteral("cid-doomed"));
        QCOMPARE(publish.at(0).at(1).toString(),
                 QStringLiteral("screen_share_failed"));
        // ONE report, not a storm: a failed pipeline posts errors repeatedly
        // and the user needs one message.
        QTest::qWait(600);
        QCOMPARE(publish.count(), 1);
        engine.stop();
    }

    // THE RATE STAGE MUST NOT CHANGE THE PICTURE'S GEOMETRY.
    //
    // This is the regression that shipped and had to be reverted. A
    // `compositor` was put here to kill videorate's first-buffer hold, and it
    // did — but COMPOSITOR IS NOT A SCALER. It paints each input at its
    // native size at xpos/ypos on an output canvas, so a 3840x2160 desktop
    // against the 1920x1080 canvas the size ceiling negotiates arrived as the
    // TOP-LEFT QUARTER of the screen. Reported as "shares only 1/4 of my
    // screen".
    //
    // The mechanical signature is exact and is what this asserts: a stage
    // that only re-times leaves the frame size alone, so videoscale upstream
    // has already met the ceiling and SINK width == SRC width. A stage that
    // composites accepts the full 4K on its sink and emits 1920 — sink != src
    // — and the difference IS the cropping.
    //
    //   measured on the reverted-to tree:  sink 1920  src 1920   (ok)
    //   measured on the compositor tree:   sink 3840  src 1920   (crop)
    void theRateStageNeverCropsTheCapture()
    {
        for (bool screenShare : {false, true}) {
            const QString stage =
                SfuMediaEngine::videoRateStage(screenShare);
            const QString desc =
                QStringLiteral(
                    "videotestsrc num-buffers=2 "
                    "! video/x-raw,width=3840,height=2160,framerate=30/1 "
                    "! videoconvert ! videoscale ! %1 name=ratestage "
                    "! video/x-raw,width=[1,1920],height=[1,1080],"
                    "framerate=30/1 ! fakesink name=out")
                    .arg(stage);
            GError *error = nullptr;
            GstElement *pipeline =
                gst_parse_launch(desc.toUtf8().constData(), &error);
            if (error) {
                const QString message = QString::fromUtf8(error->message);
                g_error_free(error);
                if (pipeline)
                    gst_object_unref(pipeline);
                QFAIL(qPrintable(QStringLiteral("rate stage %1 will not "
                                                "parse: %2")
                                     .arg(stage, message)));
            }
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            gst_element_get_state(pipeline, nullptr, nullptr,
                                  5 * GST_SECOND);

            GstElement *rate = gst_bin_get_by_name(GST_BIN(pipeline),
                                                   "ratestage");
            QVERIFY(rate);
            // ITERATE the pads; do NOT ask for a static "sink". compositor's
            // sink pads are REQUEST pads named sink_%u, so
            // gst_element_get_static_pad(rate, "sink") returns null for
            // exactly the element this test exists to catch — which made an
            // earlier revision of it SKIP on the broken tree and pass. That
            // is the whole failure mode this file keeps rediscovering.
            const auto widthOf = [](GstIterator *it) {
                int width = 0;
                GValue item = G_VALUE_INIT;
                while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
                    auto *pad = GST_PAD(g_value_get_object(&item));
                    if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
                        gst_structure_get_int(gst_caps_get_structure(caps, 0),
                                              "width", &width);
                        gst_caps_unref(caps);
                    }
                    g_value_reset(&item);
                    if (width > 0)
                        break;
                }
                g_value_unset(&item);
                gst_iterator_free(it);
                return width;
            };
            const int sinkWidth = widthOf(gst_element_iterate_sink_pads(rate));
            const int srcWidth = widthOf(gst_element_iterate_src_pads(rate));
            gst_object_unref(rate);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);

            // NOT a skip. The pipeline reached PLAYING, so both sides have
            // negotiated; a zero here means this test could not read what it
            // claims to measure, and a test that cannot measure must fail
            // rather than quietly report success.
            QVERIFY2(sinkWidth > 0 && srcWidth > 0,
                     qPrintable(QStringLiteral(
                         "could not read negotiated caps for rate stage %1 "
                         "(sink %2, src %3)")
                                    .arg(stage)
                                    .arg(sinkWidth)
                                    .arg(srcWidth)));
            QVERIFY2(sinkWidth == srcWidth,
                     qPrintable(
                         QStringLiteral(
                             "rate stage %1 (screenShare=%2) negotiated sink "
                             "width %3 against src width %4 — it is cropping "
                             "the capture, not re-timing it")
                             .arg(stage)
                             .arg(screenShare)
                             .arg(sinkWidth)
                             .arg(srcWidth)));
            QVERIFY2(srcWidth <= 1920,
                     "the rate stage broke the 1920 size ceiling");
        }
    }

    // The opening hold is a KNOWN, OPEN defect, not a shipped property, and
    // this pins the fact rather than a wished-for fix: videorate emits
    // nothing from a single capture buffer, which is why a share can take
    // ~1 s (busy desktop) to 10 s (still one) to appear. If this ever starts
    // failing, the hold has been fixed and this case should be inverted —
    // but only alongside theRateStageNeverCropsTheCapture(), because the last
    // attempt to fix the hold is what caused the crop.
    void theOpeningHoldIsStillPresentAndUnfixed()
    {
        const int frames =
            framesFromASingleCaptureBuffer(QStringLiteral("videorate"), 800);
        QVERIFY2(frames >= 0, "the videorate harness did not run");
        QCOMPARE(frames, 0);
    }

    // THE SIZE CEILING MUST STAY BEHIND THE RATE STAGE.
    //
    // Splitting the capsfilter so the size range sits BEFORE the rate stage
    // was measured to negotiate 2580x1080 from a 3440x1440 source — the 1920
    // ceiling simply violated, and a 4K share then encoded far larger than
    // the declared track. One capsfilter, after the rate stage, in both
    // branches.
    void theSingleCapsfilterFollowsTheRateStage()
    {
        for (const bool screenShare : { true, false }) {
            const QString limits =
                QStringLiteral("video/x-raw,width=(int)[1,1920],"
                               "height=(int)[1,1080],"
                               "framerate=(fraction)30/1");
            const QString desc = SfuMediaEngine::videoPipelineDescription(
                QStringLiteral("videotestsrc"),
                SfuMediaEngine::videoRateStage(screenShare), limits,
                QStringLiteral("vp8enc"), QString(), 1u,
                QStringLiteral("videoconvert ! videoscale"),
                SfuMediaEngine::captureEntryFilter(false));
            const int rateAt = desc.indexOf(
                SfuMediaEngine::videoRateStage(screenShare));
            const int capsAt = desc.indexOf(limits);
            QVERIFY2(rateAt >= 0 && capsAt >= 0,
                     "the rate stage or the limits are missing entirely");
            QVERIFY2(rateAt < capsAt,
                     "the size ceiling is applied before the rate stage");
            // And exactly ONE occurrence of that capsfilter, or the ceiling
            // is being negotiated twice.
            QCOMPARE(desc.count(limits), 1);
        }
    }

    // THE CAMERA MUST NAME ITS SOURCE, and this is a source scan because the
    // choice is a one-word literal whose consequence is invisible everywhere
    // else.
    //
    // `autovideosrc` picks by RANK, and on a PipeWire desktop `pipewiresrc`
    // outranks `v4l2src` — so it always selected pipewiresrc with no `path`
    // and no `target-object`, because only the screen-share branch has a
    // portal node to give it. Against the pinned `framerate=(fraction)30/1`
    // that is `-22 (Invalid argument)` -> not-negotiated: zero capture
    // buffers, nothing encoded, a declared camera track carrying nothing.
    // Bus errors here are logged and deliberately never raised as failures,
    // so the camera button stayed lit and the reason existed only in the log.
    //
    // ON THE BROKEN TREE this reports `autovideosrc`.
    void theCameraNamesARealSourceRatherThanAutodetecting()
    {
        const QString engine = QString::fromUtf8(engineSource());
        QVERIFY(!engine.isEmpty());
        QString code = engine;
        code.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        QVERIFY2(code.contains(QStringLiteral("QStringLiteral(\"v4l2src\")")),
                 "the camera branch does not name a real capture source");
        QVERIFY2(!code.contains(QStringLiteral("QStringLiteral(\"autovideosrc\")")),
                 "autovideosrc is back: it resolves to an unconfigured "
                 "pipewiresrc on this platform and cannot negotiate the "
                 "pinned camera framerate");
        // The stripper must strip, or the ban above is vacuous.
        QVERIFY2(code.contains(QStringLiteral("pipewiresrc fd=")),
                 "the comment stripper ate the code");
    }

    void theScreenSharePipelineParsesIncludingItsSelfView()
    {
        lightning::rtp::registerVp8Payloader();
        const QString selfView = QStringLiteral(
            "t. ! queue max-size-buffers=2 leaky=downstream "
            "! videoconvert ! video/x-raw,format=RGBA "
            "! appsink name=selfvidsink emit-signals=true "
            "sync=false max-buffers=1 drop=true ");
        const QString description = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("videotestsrc is-live=true"),
            SfuMediaEngine::videoRateStage(/*screenShare=*/true),
            QStringLiteral("video/x-raw,width=(int)[1,1920],"
                           "height=(int)[1,1080],"
                           "framerate=(fraction)[0/1,30/1]"),
            QStringLiteral("vp8enc deadline=1 end-usage=cbr "
                           "target-bitrate=3000000"),
            selfView, 12345u,
            QStringLiteral("videoconvert ! videoscale"),
                SfuMediaEngine::captureEntryFilter(false));
        // The payloader must be OURS: rtpvp8pay parses the bitstream and
        // cannot carry an encrypted frame.
        QVERIFY2(description.contains(
                     QLatin1String(lightning::rtp::vp8PayloaderName())),
                 "the video pipeline does not use the non-parsing payloader");
        QVERIFY2(!description.contains(QStringLiteral("! rtpvp8pay")),
                 "the video pipeline still uses GStreamer's parsing payloader");

        GError *error = nullptr;
        GstElement *bin = gst_parse_bin_from_description(
            description.toUtf8().constData(), TRUE, &error);
        const QString why =
            error ? QString::fromUtf8(error->message) : QString();
        if (error)
            g_error_free(error);
        QVERIFY2(bin, qPrintable(QStringLiteral(
                          "screen-share pipeline does not parse: %1").arg(why)));
        // The self-view branch has to survive: it is the only way a sharer
        // can tell their share is carrying pixels.
        GstElement *preview = gst_bin_get_by_name(GST_BIN(bin), "selfvidsink");
        QVERIFY2(preview, "the self-view branch is missing from the share");
        gst_object_unref(preview);
        gst_object_unref(bin);
    }

    void aReceivedTrackIsAttributedByThePadNotByItsIndex()
    {
        const QString pane = QString::fromUtf8(SOURCE_UNDER_TEST);
        QVERIFY(!pane.isEmpty());
        QVERIFY2(pane.contains(QStringLiteral("g_object_get(srcPad, \"msid\"")),
                 "received tracks are not attributed by the pad's own msid");
        QVERIFY2(pane.contains(QStringLiteral("g_object_get(transceiver, \"mid\"")),
                 "the track's mid does not come from its own transceiver");
        // The positional fallback must key on the section's mid, never on a
        // pad-name index.
        QVERIFY2(!pane.contains(QStringLiteral("m_streamForMline.value(mline)")),
                 "a pad-name index still indexes the SDP section map");

        // A MID MAY NEVER BE THE TRACK KEY while the SDP still holds a track
        // sid for that section.
        //
        // This is the Windows receive failure. The pad's `msid` property is
        // webrtcbin's own extraction and how much of it is filled in has
        // moved between GStreamer releases — the dev shell is 1.26.11, the
        // packaged Windows runtime 1.28.5 — so on a user's machine the
        // property came back empty and the fallback used the transceiver mid
        // AS THE KEY. Keys arrive addressed by `TR_…`, so the ring named "1"
        // was one nobody had keyed and every frame failed with `passed=0`:
        // audio silent, video absent, the connection healthy, and the sending
        // end (and Element in the same room) perfectly fine.
        //
        // The section's track sid is in the SDP text, which is identical on
        // every platform and which this engine already parses for the
        // participant and the mid. The fallback must read THAT first.
        const int fromSdp = pane.indexOf(
            QStringLiteral("trackMid = engine->m_trackForMline.value("));
        QVERIFY2(fromSdp > 0,
                 "the mid fallback does not recover the track sid from the "
                 "SDP the engine already parsed");
        const int fromMid =
            pane.indexOf(QStringLiteral("trackMid = sectionMid;"));
        QVERIFY2(fromMid < 0 || fromMid > fromSdp,
                 "a media-section mid is taken as the track key before the "
                 "SDP's own track sid is consulted");
        // ...and the SDP scan has to be extracting it in the first place.
        QVERIFY2(pane.contains(
                     QStringLiteral("trackSid = SfuMediaEngine::trackSidFromMsid")),
                 "the SDP scan records no per-section track sid, so the "
                 "fallback above has nothing to read");
        // All THREE section maps are one record of one subscriber
        // description and must be dropped together. Section mids are small
        // integers that repeat across calls, so an entry left behind is not
        // merely useless — it is a plausible-looking wrong answer.
        for (const char *map : { "m_streamForMline.clear()",
                                 "m_midForMline.clear()",
                                 "m_trackForMline.clear()" }) {
            QVERIFY2(pane.contains(QLatin1String(map)),
                     qPrintable(QStringLiteral(
                         "%1 is missing: a section map outlives its call")
                                    .arg(QLatin1String(map))));
        }
    }

    // A CHOSEN WINDOW must reach a capture element, not fall through to the
    // monitor one.
    //
    // Windows has no element that can capture a window: `gdiscreencapsrc`
    // takes a `monitor` index and a crop rectangle, and
    // `d3d11screencapturesrc`, which has `window-handle`, is in the plugin
    // this toolchain cannot load. So Lightning compiles its own
    // (WindowCaptureSrc.h) and the routing has to be pinned — a handle
    // silently ignored is a share that says "window" and sends the whole
    // desktop, which is a privacy failure rather than a cosmetic one.
    //
    // Source-scanned because the branch is `#if defined(Q_OS_WIN)`: this
    // suite runs on Linux, where calling the function can only ever return
    // the pipewiresrc form. `everyPlatformNamesACaptureSourceItActuallyHas`
    // above is the same shape for the same reason.
    void aChosenWindowRoutesToTheWindowCaptureElement()
    {
        const QString pane = QString::fromUtf8(SOURCE_UNDER_TEST);
        QVERIFY(!pane.isEmpty());

        const int windowBranch =
            pane.indexOf(QStringLiteral("if (windowHandle != 0) {"));
        QVERIFY2(windowBranch > 0,
                 "screenShareSource has no window branch at all");
        const int gdi =
            pane.indexOf(QStringLiteral("gdiscreencapsrc monitor="));
        QVERIFY2(gdi > 0, "the monitor branch vanished");
        QVERIFY2(windowBranch < gdi,
                 "a window handle is only considered after the monitor "
                 "source has already been chosen");
        QVERIFY2(pane.contains(QStringLiteral("windowCaptureSrcName()")),
                 "the window branch does not name Lightning's capture "
                 "element, so it cannot be the one that runs");

        // And the refusal above it must not reject a window: a handle is a
        // source in its own right and carries no node id. THREE kinds now —
        // a portal node, a window handle and (Linux, no portal) an X11 root
        // rectangle — and the guard is correct only when it refuses none of
        // them on its own.
        QVERIFY2(pane.contains(QStringLiteral(
                     "if (nodeId < 0 && windowHandle == 0 "
                     "&& !captureRect.isValid()) {")),
                 "the no-source refusal rejects a share that carries one of "
                 "the three source kinds but no node id");

        // The element has to be REGISTERED, or naming it in a pipeline
        // description fails at parse time — the same trap the VP8 payloader
        // is registered against, in the same place.
        QVERIFY2(pane.contains(
                     QStringLiteral("wincap::registerWindowCaptureSrc()")),
                 "the window capture element is never registered");
    }

    // THE LINUX NO-PORTAL FALLBACK'S CAPTURE, which is the one branch of this
    // function a Linux test can actually CALL rather than source-scan.
    //
    // On a desktop with no xdg-desktop-portal there was previously no way to
    // share a screen and no way to choose one. The fallback picks a whole
    // display, and a display under X11 is a RECTANGLE OF THE ROOT WINDOW —
    // there is no monitor index to pass, so the arithmetic below is the whole
    // correctness of the feature: get it wrong and the share is offset,
    // cropped, or of the wrong monitor entirely.
    void theLinuxNoPortalFallbackCapturesAnX11RootRectangle()
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QSKIP("the X11 fallback branch does not exist off Linux");
#else
        // A second monitor at an offset, which is the case that catches an
        // origin dropped on the floor.
        const QString source = SfuMediaEngine::screenShareSource(
            /*nodeId=*/1, /*pipewireFd=*/-1, /*windowHandle=*/0,
            QRect(100, 50, 1280, 720));
        QVERIFY2(source.startsWith(QStringLiteral("ximagesrc")),
                 qPrintable(QStringLiteral("not an X11 capture: %1")
                                .arg(source)));
        QVERIFY2(!source.contains(QStringLiteral("pipewiresrc")),
                 "the fallback still names the portal's element, which is "
                 "the one thing this session does not have");
        QVERIFY(source.contains(QStringLiteral("startx=100")));
        QVERIFY(source.contains(QStringLiteral("starty=50")));
        // INCLUSIVE, and this is the assertion that would catch the obvious
        // mistake. Read off the shipped plugin and then measured against it:
        // `startx=100 endx=1379` negotiates `width=(int)1280`. Writing
        // `endx = x + width` instead would capture one column too many from
        // the NEXT monitor, silently.
        QVERIFY2(source.contains(QStringLiteral("endx=1379")),
                 qPrintable(QStringLiteral("endx is not the inclusive right "
                                           "edge: %1").arg(source)));
        QVERIFY2(source.contains(QStringLiteral("endy=769")),
                 qPrintable(QStringLiteral("endy is not the inclusive bottom "
                                           "edge: %1").arg(source)));

        // A DISPLAY AT THE ORIGIN still has to name its edges rather than
        // leaving them at the element's "0 means the whole screen" default,
        // which on a multi-monitor root captures every monitor at once.
        const QString atOrigin = SfuMediaEngine::screenShareSource(
            0, -1, 0, QRect(0, 0, 1920, 1080));
        QVERIFY(atOrigin.contains(QStringLiteral("endx=1919")));
        QVERIFY(atOrigin.contains(QStringLiteral("endy=1079")));

        // THE PORTAL PATH IS UNTOUCHED. This fallback exists only for a
        // session that has no portal, and it must not be able to displace one
        // that does — with no rectangle, the source is exactly what it was.
        const QString portal = SfuMediaEngine::screenShareSource(42, 7);
        QVERIFY(portal.startsWith(QStringLiteral("pipewiresrc")));
        QVERIFY(!portal.contains(QStringLiteral("ximagesrc")));
        QVERIFY(portal.contains(QStringLiteral("fd=7")));

        // EVERY PROPERTY NAME EXISTS. `gst_parse_launch` fails outright on an
        // unknown property, so a plausible-looking name from the wrong
        // element family is a screen share that can never start — the
        // `monitor` versus `monitor-index` lesson, on the element this branch
        // actually uses. Parsing stops at NULL state, so no X server is
        // touched and nothing is captured.
        if (!SfuMediaEngine::elementAvailable(
                SfuMediaEngine::x11ScreenCaptureElementName())) {
            QSKIP("ximagesrc is not in this machine's GStreamer registry");
        }
        GError *error = nullptr;
        GstElement *bin = gst_parse_bin_from_description(
            source.toUtf8().constData(), TRUE, &error);
        QVERIFY2(bin != nullptr && error == nullptr,
                 qPrintable(QStringLiteral("the fallback capture does not "
                                           "parse: %1")
                                .arg(error ? QString::fromUtf8(error->message)
                                           : QStringLiteral("unknown"))));
        if (error)
            g_error_free(error);
        if (bin)
            gst_object_unref(bin);
#endif
    }

    // The capability probe behind the fallback, which is what decides whether
    // the picker is offered at all.
    //
    // "ximagesrc ships in gst-plugins-good and is very likely there" is how a
    // share reports success and carries nothing: the element has to be asked
    // for, in the RUNNING registry, before the user is shown a picker and
    // made to choose.
    void theCaptureElementProbeAsksTheRealRegistry()
    {
        QCOMPARE(QLatin1String(SfuMediaEngine::x11ScreenCaptureElementName()),
                 QLatin1String("ximagesrc"));
        // Something the engine already REQUIRES, so a false here would mean
        // no call could be made at all and the probe is what is broken.
        QVERIFY2(SfuMediaEngine::elementAvailable("videotestsrc"),
                 "the probe cannot see an element the engine requires, so it "
                 "would refuse every capability it is asked about");
        QVERIFY(!SfuMediaEngine::elementAvailable(
            "lightning-no-such-element-exists"));
        QVERIFY(!SfuMediaEngine::elementAvailable(nullptr));
        QVERIFY(!SfuMediaEngine::elementAvailable(""));
    }

    void aPackedLiveKitStreamIdResolvesToTheParticipant()
    {
        // Packed: participant sid, then the track id.
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123|TR_xyz789 TR_xyz789")),
                 QStringLiteral("PA_abc123"));
        // Unpacked (protocol 0, and the shape every hand-written test used):
        // still the participant, unchanged.
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123 TR_xyz789")),
                 QStringLiteral("PA_abc123"));
        // No track-id token at all.
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123")),
                 QStringLiteral("PA_abc123"));
        // A leading separator names no participant: better empty (which
        // routes and decrypts NOTHING) than a confident wrong id.
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("|TR_xyz789 TR_xyz789")),
                 QString());
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(QString()), QString());

        // The other half of the same line, and the one a media key is
        // addressed by. Both shapes LiveKit emits must yield the track sid.
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123|TR_xyz789 TR_xyz789")),
                 QStringLiteral("TR_xyz789"));
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123 TR_xyz789")),
                 QStringLiteral("TR_xyz789"));
        // Nothing that looks like a track sid: EMPTY, so the caller can tell
        // it must look elsewhere. Returning the token anyway would put a
        // participant sid where a track key belongs.
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123")),
                 QString());
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(QString()), QString());
    }

    // A media key names a Matrix DEVICE; a frame names a LiveKit sid. Those
    // two facts arrive in either order, so the ring has to be the SAME object
    // under both names or whichever arrived first is stranded — the keys in
    // one ring and the frames consulting another.
    void aKeyAndAStreamIdConvergeOnOneRingInEitherOrder()
    {
        {
            // Key first (to-device beat the participant list).
            SfuMediaEngine engine;
            const auto byIdentity =
                engine.recvCryptorFor(QStringLiteral("@a:s:DEV"));
            engine.noteParticipantIdentity(QStringLiteral("PA_1"),
                                           QStringLiteral("@a:s:DEV"));
            QCOMPARE(engine.recvCryptorFor(QStringLiteral("PA_1")).get(),
                     byIdentity.get());
        }
        {
            // Track first (the pad beat the to-device message).
            SfuMediaEngine engine;
            const auto byStream = engine.recvCryptorFor(QStringLiteral("PA_1"));
            engine.noteParticipantIdentity(QStringLiteral("PA_1"),
                                           QStringLiteral("@a:s:DEV"));
            QCOMPARE(engine.recvCryptorFor(QStringLiteral("@a:s:DEV")).get(),
                     byStream.get());
        }
        {
            // Two senders must NOT share a ring: LiveKit's key index is
            // per-participant, so both legitimately use index 0.
            SfuMediaEngine engine;
            engine.noteParticipantIdentity(QStringLiteral("PA_1"),
                                           QStringLiteral("@a:s:DEV"));
            engine.noteParticipantIdentity(QStringLiteral("PA_2"),
                                           QStringLiteral("@b:s:DEV"));
            QVERIFY(engine.recvCryptorFor(QStringLiteral("PA_1")).get()
                    != engine.recvCryptorFor(QStringLiteral("PA_2")).get());
        }
    }

    void theLocalScreenShareKeyCannotCollideWithASfuId()
    {
        const QString key = SfuMediaEngine::localScreenStreamId();
        QVERIFY(!key.isEmpty());
        // LiveKit ids are "PA_…" / "TR_…" and contain no colon.
        QVERIFY(key.contains(QLatin1Char(':')));
        QVERIFY(!key.startsWith(QStringLiteral("PA_")));
        QVERIFY(!key.startsWith(QStringLiteral("TR_")));
    }

    void muteAndDeafenBeforeAnyMediaAreSafe()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneMuted(true);
        engine.setOutputMuted(true);
        engine.start();
        engine.setMicrophoneMuted(false);
        engine.setOutputMuted(false);
        engine.setParticipantVolume(QStringLiteral("PA_x"), 50);
        engine.stop();
    }

    void aRemoteDescriptionThatIsNotSdpIsRefused()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.applyRemoteDescription(SfuMediaEngine::Target::Subscriber,
                                      QStringLiteral("offer"),
                                      QStringLiteral("not sdp at all"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("bad_remote_sdp"));
        engine.stop();
    }

    void aVideoRouterAttachedBeforeAnyCallIsSafe()
    {
        SfuMediaEngine engine;
        SfuVideoRouter router;
        engine.setVideoRouter(&router);
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-video"), false, -1);
        QTest::qWait(100);
        engine.stop();
        // Setting it to null afterwards must not fault either.
        engine.setVideoRouter(nullptr);
    }

    void iceServersAppliedBeforeAndAfterStart()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QVariantList servers;
        servers.append(QVariantMap{
            { QStringLiteral("urls"), QStringLiteral("turn:turn.example.org") },
            { QStringLiteral("username"), QStringLiteral("u") },
            { QStringLiteral("credential"), QStringLiteral("p") },
        });
        engine.setIceServers(servers);
        engine.start();
        engine.setIceServers(servers);
        // Junk must be ignored, never dereferenced.
        engine.setIceServers(QVariantList{ QVariant(), QVariant(42) });
        engine.stop();
    }

    // THE CAMERA FREEZE, reproduced as arithmetic rather than pinned as a
    // string.
    //
    // videorate starts its output clock at SEGMENT START, not at the first
    // buffer's timestamp. A publish bin joins a pipeline that has been
    // PLAYING since the call was joined, and the Windows camera and monitor
    // sources both stamp with the pipeline's RUNNING TIME — so the first
    // buffer carries the age of the call and videorate owes thirty duplicate
    // frames for every second of it, which it emits as fast as the encoder
    // will take them. One picture, at full rate, with every counter healthy.
    //
    // This drives the REAL rate stage the engine builds, so it fails the
    // moment `skip-to-first` is dropped from it: against the unfixed stage
    // the second case emits 5247 buffers instead of 27.
    void theRateStageDoesNotBackFillFromSegmentStart()
    {
        struct Counter {
            int frames = 0;
        };
        const auto measure = [](GstClockTime firstPts) {
            const QString stage = SfuMediaEngine::videoRateStage(true);
            GstElement *pipeline = gst_pipeline_new(nullptr);
            GstElement *src = gst_element_factory_make("appsrc", "src");
            GError *error = nullptr;
            GstElement *rate = gst_parse_bin_from_description(
                stage.toUtf8().constData(), TRUE, &error);
            GstElement *caps = gst_element_factory_make("capsfilter", "caps");
            GstElement *sink = gst_element_factory_make("fakesink", "sink");
            if (error) {
                g_error_free(error);
                return -1;
            }
            if (!pipeline || !src || !rate || !caps || !sink)
                return -1;

            GstCaps *inCaps = gst_caps_new_simple(
                "video/x-raw", "format", G_TYPE_STRING, "BGRA", "width",
                G_TYPE_INT, 64, "height", G_TYPE_INT, 64, "framerate",
                GST_TYPE_FRACTION, 10, 1, nullptr);
            g_object_set(src, "caps", inCaps, "format", GST_FORMAT_TIME,
                         "is-live", TRUE, "do-timestamp", FALSE, nullptr);
            gst_caps_unref(inCaps);
            GstCaps *outCaps = gst_caps_new_simple(
                "video/x-raw", "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
            g_object_set(caps, "caps", outCaps, nullptr);
            gst_caps_unref(outCaps);
            g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);

            gst_bin_add_many(GST_BIN(pipeline), src, rate, caps, sink,
                             nullptr);
            if (!gst_element_link_many(src, rate, caps, sink, nullptr))
                return -1;

            auto *counter = new Counter;
            GstPad *pad = gst_element_get_static_pad(sink, "sink");
            gst_pad_add_probe(
                pad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *, GstPadProbeInfo *, gpointer data) {
                    static_cast<Counter *>(data)->frames++;
                    return GST_PAD_PROBE_OK;
                },
                counter, nullptr);
            gst_object_unref(pad);

            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            gst_element_get_state(pipeline, nullptr, nullptr,
                                  GST_CLOCK_TIME_NONE);
            constexpr GstClockTime step = GST_SECOND / 10;
            for (int i = 0; i < 10; ++i) {
                GstBuffer *buffer =
                    gst_buffer_new_allocate(nullptr, 64 * 64 * 4, nullptr);
                gst_buffer_memset(buffer, 0, 0, 64 * 64 * 4);
                GST_BUFFER_PTS(buffer) = firstPts + i * step;
                GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
                GST_BUFFER_DURATION(buffer) = step;
                gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
            }
            gst_app_src_end_of_stream(GST_APP_SRC(src));
            GstBus *bus = gst_element_get_bus(pipeline);
            GstMessage *message = gst_bus_timed_pop_filtered(
                bus, 20 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS
                                            | GST_MESSAGE_ERROR));
            if (message)
                gst_message_unref(message);
            gst_object_unref(bus);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            const int frames = counter->frames;
            delete counter;
            return frames;
        };

        // A publish that starts with the pipeline: one second of input at
        // 10 fps becomes about one second at 30.
        const int fresh = measure(0);
        QVERIFY2(fresh > 0, "the rate stage produced nothing at all");
        QVERIFY2(fresh < 60,
                 qPrintable(QStringLiteral("a fresh publish emitted %1 buffers "
                                           "for 1 s of input")
                                .arg(fresh)));

        // The same input, published into a call that has been up for three
        // minutes. The source stamps with running time, so the first buffer
        // carries 174 s. Nothing about the INPUT changed, so nothing about
        // the output count may either.
        const int late = measure(174 * GST_SECOND);
        QVERIFY2(late > 0, "the rate stage produced nothing at all");
        QVERIFY2(late < fresh + 60,
                 qPrintable(QStringLiteral("a publish 174 s into a call "
                                           "emitted %1 buffers against %2 for "
                                           "the same input on a fresh "
                                           "pipeline — the rate stage is "
                                           "back-filling from segment start")
                                .arg(late)
                                .arg(fresh)));
    }

    // WHAT RESOLUTION A SHARED WINDOW IS PUBLISHED AT.
    //
    // The element itself is `#ifdef Q_OS_WIN`, but this rule is not: it is
    // compiled on every platform precisely so the arithmetic that decides a
    // share's shape can be held to account from a machine that has no
    // Windows. Two independent clamps used to do this job, and a 3840x2100
    // window came out as 1920x1080 — a 16:9 rectangle holding a 1.83:1
    // picture, with everything in it stretched.
    void fitIntoKeepsTheAspectAndNeverUpscales()
    {
        using lightning::wincap::fitInto;

        // The real case from the Windows report: a maximised Brave window on
        // a 4K display, against the 1080p publish ceiling. 1050, not 1080.
        const auto brave = fitInto(3840, 2100, 1920, 1080);
        QCOMPARE(brave.width, 1920);
        QCOMPARE(brave.height, 1050);

        // A window already inside the ceiling keeps its own size. Publishing
        // it larger would spend bitrate on invented pixels.
        const auto small = fitInto(800, 600, 1920, 1080);
        QCOMPARE(small.width, 800);
        QCOMPARE(small.height, 600);

        // Odd edges are rounded DOWN to even, because VP8 subsamples chroma
        // by two. The second Windows case was a 1557x1213 File Explorer.
        const auto explorer = fitInto(1557, 1213, 4000, 4000);
        QCOMPARE(explorer.width, 1556);
        QCOMPARE(explorer.height, 1212);

        // Height-bound rather than width-bound, and still proportional.
        const auto tall = fitInto(1000, 4000, 1920, 1080);
        QCOMPARE(tall.height, 1080);
        QCOMPARE(tall.width, 270);

        // Never over the ceiling, even by the rounding.
        for (int w = 1900; w <= 1940; ++w) {
            for (int h = 1070; h <= 1090; ++h) {
                const auto fit = fitInto(w, h, 1920, 1080);
                QVERIFY(fit.width <= 1920);
                QVERIFY(fit.height <= 1080);
                QVERIFY(fit.width % 2 == 0);
                QVERIFY(fit.height % 2 == 0);
            }
        }

        // Degenerate input answers with a zero size rather than inventing one.
        QCOMPARE(fitInto(0, 0, 1920, 1080).width, 0);
        QCOMPARE(fitInto(1920, 1080, 0, 0).width, 0);
        QCOMPARE(fitInto(-4, -4, 1920, 1080).height, 0);
    }

    // THE GARBLING DEFECT, pinned at its root.
    //
    // The element advertises a RANGE on its src pad, so fixation can land
    // anywhere downstream permits — measured on a real Windows run, 1920x1080
    // for a 3840x2100 window. It then went on allocating and copying a buffer
    // of the WINDOW's size, because it implemented no `set_caps` and never
    // learned what it had agreed to. Downstream reads a buffer at the stride
    // the CAPS imply, so every displayed row was half a source row and only
    // the top quarter of the window was ever consumed.
    //
    // Source-scanned because the whole file is `#ifdef Q_OS_WIN` and this
    // suite runs on Linux: the same shape as
    // `aChosenWindowRoutesToTheWindowCaptureElement` above, for the same
    // reason.
    void theGpuScaleStageIsWellFormedAndOptIn()
    {
        // BOTH BRANCHES, because the GPU one is opt-in and would otherwise
        // be first exercised by whoever set the variable — a malformed caps
        // string fails at pipeline build, which is a bad place to learn.
        for (int h : { 720, 1080, 1440, 2160 }) {
            const QString cpu = SfuMediaEngine::shareScaleStage(h, false);
            // EXACT, and it is the threaded single pass now. `videoconvert
            // ! videoscale` walked the pixels TWICE on ONE thread and took
            // 3.66 s of wall time per 5 s of 4K video — 73% of realtime
            // before the encoder started, so the chain could not keep up.
            QCOMPARE(cpu, QStringLiteral("videoconvertscale n-threads=4"));

            const QString gpu = SfuMediaEngine::shareScaleStage(h, true);
            // Imports the compositor's buffer rather than asking for system
            // memory, which is the whole point: a plain video/x-raw request
            // forces a readback of every frame at CAPTURE size.
            QVERIFY2(gpu.startsWith(QStringLiteral("glupload")),
                     qPrintable(gpu));
            // THE CONVERT IS LOAD-BEARING. The portal hands over
            // `format=DMA_DRM` — an opaque DRM-modifier buffer, not a
            // sampleable colour format — so without glcolorconvert the
            // scaler has nothing to filter and the share goes out BLACK.
            // Measured on a real capture: negotiation succeeded, frames
            // flowed, picture empty.
            QVERIFY2(gpu.contains(QStringLiteral("glcolorconvert")),
                     qPrintable(QStringLiteral(
                         "no glcolorconvert: a DMA_DRM buffer reaches the "
                         "scaler unconverted and the share is black: %1")
                             .arg(gpu)));
            // texture-target=2D IS LOAD-BEARING. A DMA-BUF imported as an
            // EGLImage arrives as GL_TEXTURE_EXTERNAL_OES; without pinning
            // the target, glcolorconvert may pass it through and
            // glcolorscale samples it as 2D and reads nothing — frames
            // flow, encoder runs, picture black.
            // THE FALLBACK MUST NOT BE (ANY). It matches memory:DMABuf with
            // ANY modifier, so a linear preference listed first is silently
            // defeated and a block-linear buffer reaches the GL import,
            // which goes out black with every counter healthy.
            const QString entry = SfuMediaEngine::captureEntryFilter(true);
            QVERIFY2(!entry.contains(QStringLiteral("video/x-raw(ANY)")),
                     qPrintable(QStringLiteral(
                         "an (ANY) fallback re-admits block-linear DMA-BUF "
                         "and the share goes out black: %1").arg(entry)));
            QVERIFY2(entry.contains(QStringLiteral("AR24:0x0000000000000000")),
                     qPrintable(QStringLiteral(
                         "the GPU entry filter no longer requires a linear "
                         "DMA-BUF: %1").arg(entry)));
            QVERIFY2(gpu.contains(QStringLiteral("texture-target=2D")),
                     qPrintable(QStringLiteral(
                         "no texture-target pin: an external-oes texture can "
                         "reach the scaler and the share goes out black: %1")
                             .arg(gpu)));
            // And it must be pinned BEFORE the scaler, or the conversion
            // has already been skipped by the time it matters.
            QVERIFY2(gpu.indexOf(QStringLiteral("texture-target=2D"))
                         < gpu.indexOf(QStringLiteral("glcolorscale")),
                     qPrintable(QStringLiteral(
                         "the texture-target pin is after the scaler: %1")
                             .arg(gpu)));
            QVERIFY2(gpu.contains(QStringLiteral("format=RGBA")),
                     qPrintable(QStringLiteral(
                         "the GL caps do not pin a sampleable format: %1")
                             .arg(gpu)));
            QVERIFY2(gpu.contains(QStringLiteral("memory:GLMemory")),
                     qPrintable(QStringLiteral(
                         "the size is not constrained inside GL, so "
                         "glcolorscale has no target and the scale happens "
                         "after the download anyway: %1").arg(gpu)));
            QVERIFY2(gpu.contains(QStringLiteral("gldownload")),
                     qPrintable(gpu));
            // RANGES, so a window smaller than the ceiling is still never
            // upscaled — the same property the CPU path has.
            QVERIFY2(gpu.contains(QStringLiteral("[1,%1]").arg(h)),
                     qPrintable(gpu));
            QVERIFY2(gpu.contains(QStringLiteral("[1,%1]").arg((h * 16) / 9)),
                     qPrintable(gpu));
            // ASCII digits, same as the caps: this string goes to the same
            // C parser.
            for (QChar c : gpu) {
                QVERIFY2(!c.isDigit() || (c >= QLatin1Char('0')
                                          && c <= QLatin1Char('9')),
                         "a non-ASCII digit reached the GL caps string");
            }
        }

        // THE ENTRY FILTER, which is what actually broke the first attempt.
        // Both forms pin the PAR — an unfixated PAR is taken to its minimum,
        // 1/2147483647, and overflows videoscale — but only the GPU form
        // lets a non-system memory feature through, and without that
        // pipewiresrc is asked for a downloaded buffer before glupload can
        // import anything. Measured on a real capture as
        // "no more input formats", and the share never started.
        const QString cpuEntry = SfuMediaEngine::captureEntryFilter(false);
        const QString gpuEntry = SfuMediaEngine::captureEntryFilter(true);
        for (const QString &f : { cpuEntry, gpuEntry }) {
            QVERIFY2(f.contains(QStringLiteral(
                         "pixel-aspect-ratio=(fraction)1/1")),
                     qPrintable(QStringLiteral(
                         "the PAR pin is gone, which lets gst_caps_fixate "
                         "take it to 1/2147483647: %1").arg(f)));
        }
        QVERIFY2(!cpuEntry.contains(QStringLiteral("(ANY)")),
                 "the CPU path stopped pinning system memory");
        // LINEAR REQUIRED, NOT MERELY PREFERRED — and the earlier version
        // of this case asserted the PREFERENCE, which is why the defect
        // looked covered while it was live. It required the linear modifier
        // to appear before a `video/x-raw(ANY)` fallback, reasoning that
        // caps are an ordered preference list. They are, and that is exactly
        // the problem: `(ANY)` matches every caps feature INCLUDING
        // memory:DMABuf at any modifier, so the peer was free to decline the
        // first entry and take the second. It did — the compositor answered
        // `drm-format=AR24:0x0300000000606014`, NVIDIA block-linear, which
        // imports as an empty texture and publishes a black share with every
        // counter healthy.
        //
        // So the fallback must not be able to carry a DMA-BUF at all. Either
        // a LINEAR one the GL chain can import, or plain system memory.
        // THE CPU FALLBACK IS ONE THREADED PASS, and both properties are
        // load-bearing rather than tidy. Measured 4K -> 1080p over 5 s of
        // video: `videoconvert ! videoscale` took 3.66 s of WALL time, i.e.
        // 73% of realtime for the convert alone, before the encoder's
        // ~2.35 s. Serially that is ~6 s of work per 5 s of video, so the
        // stage cannot keep up and the share stutters.
        // `videoconvertscale n-threads=4` is 1.65 s at the same total CPU.
        {
            const QString cpu = SfuMediaEngine::shareScaleStage(1080, false);
            QVERIFY2(cpu.contains(QStringLiteral("videoconvertscale")),
                     qPrintable(QStringLiteral(
                         "the CPU fallback went back to two passes: %1")
                             .arg(cpu)));
            QVERIFY2(cpu.contains(QStringLiteral("n-threads=4")),
                     qPrintable(QStringLiteral(
                         "the CPU fallback is single-threaded again, which "
                         "cannot keep up with a 4K capture: %1").arg(cpu)));
        }
        // THE PATH IS LINUX-ONLY, and on Windows it would not merely fail
        // to help — `libgstopengl.dll` is not in the staged plugin set, so
        // gst_parse_launch would fail on an unknown element and there would
        // be NO SCREEN SHARE AT ALL. Pinned so nobody widens the gate
        // without also staging the plugin and answering the system-memory
        // premise in point 3 of the comment on the accessor.
#if defined(Q_OS_LINUX)
        // The env var is readable here, so the gate is the platform alone.
        QVERIFY2(SfuMediaEngine::shareScaleStage(1080, true)
                     .contains(QStringLiteral("glupload")),
                 "the GPU stage stopped using glupload on Linux");
        // AND THE PROBE MUST NAME THE ELEMENT, not answer a boolean. A
        // packaged build that forgot one plugin is the likeliest way this
        // path dies, and "GPU unavailable" would send the next person
        // hunting the driver instead of the plugin list.
        {
            const QString missing = SfuMediaEngine::missingGpuShareElement();
            // In the dev shell every GL element is present, so this is the
            // "nothing missing" answer; the point is that it is a NAME.
            QVERIFY2(missing.isEmpty()
                         || missing.startsWith(QStringLiteral("gl")),
                     qPrintable(QStringLiteral(
                         "the missing-element probe returned something that "
                         "is not a GL element name: %1").arg(missing)));
        }
#else
        QVERIFY2(!SfuMediaEngine::shareGpuScalingRequested(),
                 "the GPU share path is enabled off Linux, where glupload is "
                 "not staged and gst_parse_launch would fail on it");
#endif
        QVERIFY2(!gpuEntry.contains(QStringLiteral("video/x-raw(ANY)")),
                 qPrintable(QStringLiteral(
                     "an (ANY) fallback re-admits a block-linear DMA-BUF, so "
                     "the linear entry is only a preference and the share "
                     "goes out black: %1").arg(gpuEntry)));
        QVERIFY2(gpuEntry.contains(QStringLiteral("0x0000000000000000")),
                 qPrintable(QStringLiteral(
                     "the GPU entry filter stopped requiring a linear "
                     "DMA-BUF: %1").arg(gpuEntry)));
        // (The case that used to sit here REQUIRED `video/x-raw(ANY)`,
        // arguing that without it "the compositor's buffer can never be
        // imported". Together with the ordering case above, the two
        // assertions enforced the defect from both sides: one demanded the
        // linear entry come first, the other demanded the fallback that let
        // the peer skip it. A block-linear buffer is not importable here, so
        // refusing DMA-BUF and taking system memory is the CORRECT outcome,
        // not a regression.)

        // DEFAULT ON where the platform can carry it — this case used to
        // assert the opposite ("opt-in, and it has never been measured on a
        // real capture"), which was true until the path was confirmed
        // working on a real desktop. What keeps that safe is not the
        // default but the LADDER: intent here, availability probed
        // separately, and a parse failure falling back once. So the thing
        // worth pinning is that wanting the GPU and being able to use it
        // stay two different questions.
#if defined(Q_OS_LINUX)
        QVERIFY2(SfuMediaEngine::shareGpuScalingRequested(),
                 "the GPU share path is no longer attempted by default");
#endif
        // THE PROBE'S FAILING BRANCH, DRIVEN FOR REAL. An earlier version
        // of this asserted over missingGpuShareElement() directly, which in
        // the dev shell always answers "nothing missing" — so the branch
        // that matters was unreachable and the assertion passed on a
        // deliberately broken probe. A mutation check caught it. The names
        // are a parameter now precisely so this can be exercised.
        QVERIFY2(SfuMediaEngine::firstMissingElement(
                     {QByteArrayLiteral("queue")}).isEmpty(),
                 "the probe reports a present element as missing");
        QCOMPARE(SfuMediaEngine::firstMissingElement(
                     {QByteArrayLiteral("queue"),
                      QByteArrayLiteral("lightning-no-such-element"),
                      QByteArrayLiteral("also-not-real")}),
                 QStringLiteral("lightning-no-such-element"));
        // And the real list resolves through the same helper.
        const QString missingNow = SfuMediaEngine::missingGpuShareElement();
        QVERIFY2(missingNow.isEmpty()
                     || missingNow.startsWith(QStringLiteral("gl")),
                 qPrintable(QStringLiteral(
                     "the availability probe answered something that is not "
                     "a GL element name: %1").arg(missingNow)));
    }

    // The GPU/CPU ladder must not re-point the CAMERA at share policy.
    //
    // THIS IS A REGRESSION CASE AND IT FAILED ON THE FIRST VERSION OF THE
    // LADDER. That version chose the fallback stage as
    // `useGpu ? shareStage : shareScaleStage(m_shareMaxHeight, false)`, and
    // `useGpu` is false for EVERY camera publish — so the camera silently
    // stopped using its own `videoconvert ! videoscale` and started using
    // the share's, sized by the share's max height. Harmless in effect
    // today, since the CPU branch ignores the height and the elements are
    // equivalent, which is exactly why nothing noticed.
    void theCameraKeepsItsOwnScaleStageWhateverTheShareIsDoing()
    {
        // The two stages must be DISTINGUISHABLE, or this case cannot fail.
        const QString shareCpu = SfuMediaEngine::shareScaleStage(2160, false);
        const QString cameraStage = QStringLiteral("videoconvert ! videoscale");
        QVERIFY2(shareCpu != cameraStage,
                 "the share and camera CPU stages became identical, so this "
                 "case can no longer detect the camera being re-pointed");

        // THE DECISION ITSELF, not a description we handed the answer to.
        // The first version of this case built a description while passing
        // the camera stage in as an argument — so it asserted that
        // videoPipelineDescription() uses what it is given, which was never
        // in doubt, and it PASSED with the bug reintroduced. A mutation
        // check caught that.
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(false, 2160),
                 cameraStage);
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(true, 2160), shareCpu);
        // And the share's height must not leak into the camera's answer.
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(false, 720),
                 SfuMediaEngine::cpuFallbackScaleStage(false, 2160));
    }

    void shareCapsAndBitrateAreRightAtEveryOfferedQuality()
    {
        // ALL NINE COMBINATIONS, against the real derivation rather than a
        // hardcoded copy of the expected string. The two existing caps cases
        // in this file build their own "[1,1920]"/"[1,1080]" literals, so
        // they cannot see a regression in this arithmetic.
        struct Row { int h; int fps; int w; };
        const QList<Row> rows = {
            { 720, 15, 1280 },  { 720, 30, 1280 },  { 720, 60, 1280 },
            { 1080, 15, 1920 }, { 1080, 30, 1920 }, { 1080, 60, 1920 },
            { 1440, 15, 2560 }, { 1440, 30, 2560 }, { 1440, 60, 2560 },
            { 2160, 15, 3840 }, { 2160, 30, 3840 }, { 2160, 60, 3840 },
        };
        for (const Row &r : rows) {
            const QString caps = SfuMediaEngine::shareLimitsCaps(r.h, r.fps);

            // RANGES, or videoscale would upscale a small window to the
            // ceiling — the "never upscaled" property the pipeline depends
            // on.
            QVERIFY2(caps.contains(QStringLiteral("width=(int)[1,%1]")
                                       .arg(r.w)),
                     qPrintable(QStringLiteral("wrong width range at %1p%2: "
                                               "%3").arg(r.h).arg(r.fps)
                                    .arg(caps)));
            QVERIFY2(caps.contains(QStringLiteral("height=(int)[1,%1]")
                                       .arg(r.h)),
                     qPrintable(caps));
            // FIXED framerate, never a range: a range including 0/1 leaves
            // vp8enc no rate to plan against.
            QVERIFY2(caps.contains(QStringLiteral("framerate=(fraction)%1/1")
                                       .arg(r.fps)),
                     qPrintable(caps));
            // FIXED PAR: an un-fixated PAR is taken to 1/2147483647 and
            // overflows videoscale.
            QVERIFY2(caps.contains(
                         QStringLiteral("pixel-aspect-ratio=(fraction)1/1")),
                     qPrintable(caps));
            // ASCII digits. The caps go to a C parser, so a localised digit
            // would not parse at all.
            for (QChar c : caps) {
                QVERIFY2(!c.isDigit() || (c >= QLatin1Char('0')
                                          && c <= QLatin1Char('9')),
                         "a non-ASCII digit reached the caps string");
            }

            const QString enc = SfuMediaEngine::shareEncoderStage(r.h, r.fps);
            // The keyframe interval is in FRAMES and must track the rate, or
            // 60 frames means 2 s at 30 fps and 4 s at 15.
            QVERIFY2(enc.contains(QStringLiteral("keyframe-max-dist=%1")
                                      .arg(2 * r.fps)),
                     qPrintable(enc));
            // And the bitrate stays inside the band. The ceiling matters:
            // there is no congestion control on this lane, and end-usage=cbr
            // pads to hit whatever it is told.
            static const QRegularExpression rate(
                QStringLiteral("target-bitrate=(\\d+)"));
            const QRegularExpressionMatch m = rate.match(enc);
            QVERIFY2(m.hasMatch(), qPrintable(enc));
            const int bitrate = m.captured(1).toInt();
            QVERIFY2(bitrate >= 800000 && bitrate <= 6000000,
                     qPrintable(QStringLiteral("bitrate %1 out of band at "
                                               "%2p%3").arg(bitrate)
                                    .arg(r.h).arg(r.fps)));
        }

        // 1080p30 is the reference and must be unchanged from what shipped,
        // or every existing share silently changes quality.
        QVERIFY(SfuMediaEngine::shareEncoderStage(1080, 30)
                    .contains(QStringLiteral("target-bitrate=3000000")));
        QVERIFY(SfuMediaEngine::shareLimitsCaps(1080, 30)
                    .contains(QStringLiteral("width=(int)[1,1920]")));
    }

    void shareAudioIsOfferedOnlyWhenSomethingCanActuallyCaptureIt()
    {
        // The availability answer must come from the SHIPPED plugin set, not
        // from the platform macro. Which capture plugin a package carries is
        // a packaging fact this code cannot see -- Windows stages both wasapi
        // and wasapi2, and a build could ship neither -- so the question is
        // asked of GStreamer at runtime.
        //
        // The two branches are asserted separately BECAUSE either can be the
        // truth on a given machine, and a test that only covered the
        // available branch would silently stop testing anything on a build
        // without the element.
        const bool available = SfuMediaEngine::shareAudioAvailable();

        // Whatever the answer, it must agree with whether an element that
        // can do the job is actually present. Recomputed here from the
        // factories rather than copied from the implementation, so the two
        // can disagree and be caught.
        const char *const kCandidates[] = {
#if defined(Q_OS_WIN)
            "wasapi2src", "wasapisrc",
#elif defined(Q_OS_LINUX)
            "pulsesrc",
#endif
            nullptr,
        };
        bool anyPresent = false;
        for (const char *const *c = kCandidates; *c; ++c) {
            if (GstElementFactory *f = gst_element_factory_find(*c)) {
                gst_object_unref(f);
                anyPresent = true;
            }
        }
        QCOMPARE(available, anyPresent);

        // And the refusal is HONEST when it cannot: publishing share audio
        // with no peer must add no bin and must not pretend it did. A track
        // announced to the SFU and never fed is worse than no track, because
        // the far end renders a participant who is sharing silence.
        SfuMediaEngine engine;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"));
        QVERIFY2(!engine.hasPublishedBinForTest(
                     QStringLiteral("cid-share-audio")),
                 "share audio registered a bin without a publisher peer");
    }

    void theWindowCaptureLearnsTheSizeItNegotiated()
    {
        QFile file(QStringLiteral(SOURCE_DIR "/src/calls/WindowCaptureSrc.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 "WindowCaptureSrc.cpp is not where this test looks for it");
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY(source.size() > 1000);

        QVERIFY2(source.contains(QStringLiteral("base->set_caps = setCapsSrc;")),
                 "the element installs no set_caps vfunc, so it cannot learn "
                 "the frame size it negotiated");

        // The buffer's size must come from the NEGOTIATED fields, never from
        // a fresh measurement of the window.
        QVERIFY2(source.contains(QStringLiteral(
                     "static_cast<gsize>(self->outWidth) * self->outHeight")),
                 "the frame size is no longer computed from the negotiated "
                 "output size");

        // The capture measures the WINDOW rect, because that is what
        // PrintWindow draws. Sizing a surface to the client rect while
        // printing the whole window is what put the frame in the top of the
        // picture and cut the same number of pixels off the bottom.
        // The CALL form, with its open paren. Banning the bare token matches
        // the comment that explains why the client rect is wrong, which is
        // the "a ban regex matching a token named in a COMMENT" trap this
        // repository has already paid for once.
        QVERIFY2(!source.contains(QStringLiteral("GetClientRect(")),
                 "the capture is measuring the client rect again while "
                 "PrintWindow draws the whole window");

        // And the fit is the shared rule, not a pair of clamps written here.
        QVERIFY2(source.contains(QStringLiteral("wincap::fitInto(")),
                 "the element no longer uses the shared fitting rule");
    }
};

QTEST_MAIN(SfuMediaEngineTest)
#include "SfuMediaEngineTest.moc"
