// Whether per-application share audio believes a PipeWire daemon it cannot
// reach. Its own executable on purpose: the PipeWire GStreamer plugin shares
// one connection per process between every element and provider, so once any
// other case has connected, a later "no daemon" case would silently reuse that
// connection and prove nothing.
#include <QtTest/QtTest>

#include <QFile>
#include <QTemporaryDir>
#include <QDir>

#include <gst/gst.h>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "calls/GstBootstrap.h"
#include "calls/ShareAudioSources.h"

class ShareAudioDaemonTest : public QObject
{
    Q_OBJECT

private slots:
    // The PipeWire device provider reports a successful start even when it
    // reaches no daemon ("failed: return TRUE" in every version through 1.6).
    // Trusting that made a PulseAudio-only machine (Linux Mint 21's default)
    // with a PipeWire plugin look capable of per-application capture, and
    // every share then carried the silence floor alone: the far end heard
    // nothing and nothing on screen said so. Fails on the old SourceMonitor,
    // which started an unfiltered device monitor and answered true.
    void perApplicationCaptureNeedsAPipeWireDaemonItCanReach()
    {
        // Before anything initialises GStreamer or PipeWire in this process:
        // an absolute PIPEWIRE_REMOTE is the socket libpipewire connects to.
        qputenv("PIPEWIRE_REMOTE", "/nonexistent-lightning-test/pipewire-0");
        QVERIFY(lightning::gst::ensureInitialised());
        if (!gst_device_provider_factory_find("pipewiredeviceprovider"))
            QSKIP("no PipeWire device provider in this build");
        for (const char *needed : { "audiomixer", "pipewiresrc" }) {
            GstElementFactory *factory = gst_element_factory_find(needed);
            if (!factory)
                QSKIP(qPrintable(QStringLiteral("no %1 in this build")
                                     .arg(QLatin1String(needed))));
            gst_object_unref(factory);
        }
        lightning::shareaudio::SourceMonitor monitor;
        const bool started = monitor.start();
        const QList<lightning::shareaudio::Stream> streams =
            monitor.streams(QCoreApplication::applicationPid(), {});
        monitor.stop();
        QVERIFY2(!started,
                 "per-application capture reported available with no PipeWire "
                 "daemon to capture from — the share would carry silence");
        QVERIFY(streams.isEmpty());
        // And the cached, bounded answer the UI binds to agrees.
        QVERIFY2(!lightning::shareaudio::perApplicationCaptureAvailable(),
                 "the UI would offer to choose applications that cannot be "
                 "captured");
    }

    // Every enumeration and every capture branch gets a PipeWire connection
    // of its own: the plugin's shared one stays dead after a daemon restart
    // while anything holds it, and a provider started on it waited for ever
    // (measured live). The socket is found where libpipewire looks, and the
    // fd lives exactly as long as the element it was given to.
    void privateConnectionsAreFoundAndLiveAsLongAsTheirElement()
    {
#if !defined(Q_OS_UNIX) || defined(Q_OS_DARWIN)
        QSKIP("PipeWire sockets are a Linux matter");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray socketPath = QFile::encodeName(dir.filePath(
            QStringLiteral("pipewire-test")));
        const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(listener >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        QVERIFY(socketPath.size() < int(sizeof(address.sun_path)));
        std::memcpy(address.sun_path, socketPath.constData(),
                    size_t(socketPath.size()));
        QCOMPARE(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                        sizeof(address)),
                 0);
        QCOMPARE(::listen(listener, 8), 0);

        // An absolute remote is the socket itself.
        qputenv("PIPEWIRE_REMOTE", socketPath);
        int fd = lightning::shareaudio::privatePipeWireConnection();
        QVERIFY2(fd >= 0, "an absolute PIPEWIRE_REMOTE was not connected to");
        ::close(fd);
        // A name is looked up in PIPEWIRE_RUNTIME_DIR.
        qputenv("PIPEWIRE_REMOTE", "pipewire-test");
        qputenv("PIPEWIRE_RUNTIME_DIR", QFile::encodeName(dir.path()));
        fd = lightning::shareaudio::privatePipeWireConnection();
        QVERIFY2(fd >= 0, "a remote name was not found in PIPEWIRE_RUNTIME_DIR");
        ::close(fd);
        // Nothing there: no connection, and the caller keeps the shared one.
        qputenv("PIPEWIRE_REMOTE", "no-such-remote");
        QCOMPARE(lightning::shareaudio::privatePipeWireConnection(), -1);

        // Given to a pipewiresrc: set as its fd, closed once it is gone.
        qputenv("PIPEWIRE_REMOTE", socketPath);
        QVERIFY(lightning::gst::ensureInitialised());
        GstElement *source = gst_element_factory_make("pipewiresrc", nullptr);
        if (!source) {
            ::close(listener);
            QSKIP("no pipewiresrc in this build");
        }
        gst_object_ref_sink(source);
        fd = lightning::shareaudio::givePrivatePipeWireConnection(
            G_OBJECT(source));
        QVERIFY(fd >= 0);
        int property = -1;
        g_object_get(source, "fd", &property, nullptr);
        QCOMPARE(property, fd);
        QVERIFY2(::fcntl(fd, F_GETFD) != -1,
                 "the fd was closed while its element still lives");
        gst_object_unref(source);
        QVERIFY2(::fcntl(fd, F_GETFD) == -1 && errno == EBADF,
                 "the fd outlived its element: a leak per branch");
        ::close(listener);
        qunsetenv("PIPEWIRE_RUNTIME_DIR");
#endif
    }

    // The device provider is a process-wide singleton that is never
    // finalized, so a connection handed to it with a weak-ref close leaked:
    // one connected socket per pass (and a client on the daemon, which the
    // plugin's dup kept alive), ~30 a minute while a list showed or a share
    // ran — EMFILE for everything else in the process within the hour. A pass
    // owns and closes its fd. The "daemon" here accepts each connection and
    // hangs up 100 ms later, which ends the provider's start() with a core
    // error, as a real daemon going away does.
    void enumerationPassesLeakNoFileDescriptors()
    {
#if !defined(Q_OS_UNIX) || defined(Q_OS_DARWIN)
        QSKIP("PipeWire sockets are a Linux matter");
#else
        QVERIFY(lightning::gst::ensureInitialised());
        if (!gst_device_provider_factory_find("pipewiredeviceprovider"))
            QSKIP("no PipeWire device provider in this build");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray socketPath =
            QFile::encodeName(dir.filePath(QStringLiteral("pw-hangup")));
        const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        QVERIFY(listener >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socketPath.constData(),
                    size_t(socketPath.size()));
        QCOMPARE(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                        sizeof(address)),
                 0);
        QCOMPARE(::listen(listener, 16), 0);
        constexpr int kPasses = 12;
        std::thread daemon([listener] {
            for (int i = 0; i < kPasses; ++i) {
                const int peer = ::accept(listener, nullptr, nullptr);
                if (peer < 0)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ::close(peer);
            }
        });
        qputenv("PIPEWIRE_REMOTE", socketPath);
        const auto openFds = [] {
            return QDir(QStringLiteral("/proc/self/fd"))
                .entryList(QDir::NoDotAndDotDot | QDir::AllEntries)
                .size();
        };
        // One pass first: the plugin's own one-time state (its loop's
        // eventfds) is not a leak.
        std::atomic<int> done{0};
        std::thread passes([&done] {
            for (int i = 0; i < kPasses; ++i) {
                lightning::shareaudio::pipewireDevices(nullptr);
                if (i == 0)
                    done.store(1);
                while (i == 0 && done.load() != 2)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            done.store(3);
        });
        // A hung pass must fail the case, not hang the suite or terminate it
        // with a joinable thread.
        const auto waitFor = [&done](int value, int ms) {
            QElapsedTimer clock;
            clock.start();
            while (done.load() != value && clock.elapsed() < ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return done.load() == value;
        };
        if (!waitFor(1, 10000)) {
            passes.detach();
            daemon.detach();
            QFAIL("the first pass never returned");
        }
        const int before = openFds();
        done.store(2);
        if (!waitFor(3, 30000)) {
            passes.detach();
            daemon.detach();
            QFAIL("a pass never returned");
        }
        passes.join();
        daemon.join();
        const int after = openFds();
        ::close(listener);
        QVERIFY2(after <= before,
                 qPrintable(QStringLiteral("%1 descriptors leaked over %2 "
                                           "passes")
                                .arg(after - before)
                                .arg(kPasses - 1)));
#endif
    }

    // Opt-in, against a PipeWire graph YOU name (never the session's by
    // default): LIGHTNING_TEST_PIPEWIRE_REMOTE=<socket path>, with at least one
    // application playing. Enumerates over a private connection, captures the
    // first stream through a pipewiresrc on its own connection, and checks
    // audio arrives and the fd is released with the element. Run it ALONE
    // (name it on the command line): the probe answers once per process, and
    // the first case has already made it answer "no daemon".
    void aPrivateConnectionEnumeratesAndCapturesOnARealGraph()
    {
#if !defined(Q_OS_UNIX) || defined(Q_OS_DARWIN)
        QSKIP("PipeWire is Linux-only");
#else
        const QByteArray remote = qgetenv("LIGHTNING_TEST_PIPEWIRE_REMOTE");
        if (remote.isEmpty())
            QSKIP("set LIGHTNING_TEST_PIPEWIRE_REMOTE to a PipeWire socket");
        qputenv("PIPEWIRE_REMOTE", remote);
        QVERIFY(lightning::gst::ensureInitialised());
        // The probe's own pass is on a private connection too.
        QVERIFY(lightning::shareaudio::perApplicationCaptureAvailable());
        const lightning::shareaudio::Enumeration e =
            lightning::shareaudio::enumerate(QCoreApplication::applicationPid(),
                                             {}, {});
        QVERIFY2(!e.streams.isEmpty(), "no application streams on that graph");
        const QString serial = e.streams.first().serial;
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            QStringLiteral("pipewiresrc name=s target-object=%1 min-buffers=1 "
                           "! audioconvert ! capsfilter caps=audio/x-raw "
                           "! fakesink name=k sync=false")
                .arg(serial)
                .toUtf8()
                .constData(),
            &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "s");
        const int fd = lightning::shareaudio::givePrivatePipeWireConnection(
            G_OBJECT(source));
        QVERIFY(fd >= 0);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "k");
        GstPad *pad = gst_element_get_static_pad(sink, "sink");
        std::atomic<int> buffers{0};
        gst_pad_add_probe(
            pad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            },
            &buffers, nullptr);
        gst_object_unref(pad);
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QTRY_VERIFY_WITH_TIMEOUT(buffers.load() > 10, 5000);
        QVERIFY(!lightning::shareaudio::connectionClosed(fd));
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(source);
        gst_object_unref(pipeline);
        QVERIFY2(::fcntl(fd, F_GETFD) == -1 && errno == EBADF,
                 "the fd outlived its element");
#endif
    }
};

QTEST_GUILESS_MAIN(ShareAudioDaemonTest)
#include "ShareAudioDaemonTest.moc"
