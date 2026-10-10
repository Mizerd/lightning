// SfuMediaEngine tests. Everything runs in test-source mode (synthetic
// audio/video, fakesinks; no microphone, camera or display server): pipeline
// construction, the crypto pad probes, per-sender key rings and teardown.
#include "calls/SfuMediaEngine.h"
#include "calls/CaptureClock.h"

#include "calls/CallFrameCryptor.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/ShareAudioSources.h"
#include "calls/SfuVideoRouter.h"
#include "calls/WindowCaptureSrc.h"
#include "calls/noise/DenoiseElement.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QThread>
#include <QSet>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QtTest/QtTest>

#include <QFile>
#include <QUrl>
#include <QVideoSink>

#include <gst/app/gstappsrc.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video-event.h>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {

/// How many frames reach a sink when exactly one buffer is pushed into a live
/// pipeline whose caps say `framerate=0/1` (the desktop-capture shape: PipeWire
/// delivers only when the screen changes). `videorate` needs a second input
/// before it can emit anything, so it returns 0; an aggregator with its own
/// output deadline emits from the first buffer. -1 means the harness failed.
int framesFromASingleCaptureBuffer(const QString &rateStage, int waitMs)
{
    // Production shape minus the tee and encoder: only the rate stage's
    // ability to start is under test.
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

    // The portal's caps shape (BGRA, 0/1), small enough to cost nothing.
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
    // Nothing else is pushed: a still desktop.
    QTest::qWait(waitMs);
    const int frames = seen->load();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sinkPad);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return frames;
}

/// The engine's own source, for invariants about how it talks to GStreamer
/// that a behaviour test between two engines cannot see.
QByteArray engineSource()
{
    QFile file(QStringLiteral(LIGHTNING_SFU_ENGINE_SOURCE));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
#define SOURCE_UNDER_TEST engineSource()

/// The first IPv4 address of an interface that is up, running and neither
/// loopback nor link-local, or empty.
QString hostIpv4Address()
{
    ifaddrs *list = nullptr;
    if (::getifaddrs(&list) != 0)
        return {};
    QString found;
    for (ifaddrs *it = list; it && found.isEmpty(); it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET
            || !(it->ifa_flags & IFF_UP) || !(it->ifa_flags & IFF_RUNNING)
            || (it->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }
        const auto *in = reinterpret_cast<const sockaddr_in *>(it->ifa_addr);
        if ((ntohl(in->sin_addr.s_addr) >> 16) == 0xA9FE) // 169.254/16
            continue;
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &in->sin_addr, text, sizeof text))
            found = QString::fromLatin1(text);
    }
    ::freeifaddrs(list);
    return found;
}

/// A UDP port that takes packets and never answers. Named as an ICE server it
/// keeps gathering running until libnice gives up on it (about 2 s, measured
/// with libnice 0.1.23), which is the window GitHub #3 needs. On 127.0.0.1
/// it only holds gathering open: libnice pins each host candidate's socket to
/// its interface (IP_UNICAST_IF), so nothing it sends can reach loopback. To
/// SEE what libnice sends, bind it to hostIpv4Address().
class SilentUdpPort
{
public:
    explicit SilentUdpPort(const QString &address = QStringLiteral("127.0.0.1"))
        : m_address(address)
    {
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
            return;
        sockaddr_in address4{};
        address4.sin_family = AF_INET;
        if (::inet_pton(AF_INET, m_address.toLatin1().constData(),
                        &address4.sin_addr)
            != 1) {
            return;
        }
        socklen_t length = sizeof address4;
        if (::bind(m_fd, reinterpret_cast<sockaddr *>(&address4), length) == 0
            && ::getsockname(m_fd, reinterpret_cast<sockaddr *>(&address4),
                             &length)
                == 0) {
            m_port = ntohs(address4.sin_port);
        }
    }
    ~SilentUdpPort()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    int port() const { return m_port; }
    /// UDP only: with no `transport` libnice also tries TCP, whose give-up
    /// time is longer.
    QVariantList asTurnServer() const
    {
        return { QVariantMap{
            { QStringLiteral("urls"),
              QStringLiteral("turn:%1:%2?transport=udp").arg(m_address).arg(m_port) },
            { QStringLiteral("username"), QStringLiteral("u") },
            { QStringLiteral("credential"), QStringLiteral("p") },
        } };
    }
    QVariantList asStunServer() const
    {
        return { QVariantMap{
            { QStringLiteral("urls"),
              QStringLiteral("stun:%1:%2").arg(m_address).arg(m_port) },
        } };
    }
    /// Whether a STUN Binding Request (type 0x0001 with the RFC 5389 magic
    /// cookie) has arrived. Reads whatever is queued, never blocks.
    bool sawStunBindingRequest()
    {
        unsigned char datagram[1500];
        for (;;) {
            const ssize_t size =
                ::recv(m_fd, datagram, sizeof datagram, MSG_DONTWAIT);
            if (size < 0)
                break;
            if (size >= 20 && datagram[0] == 0x00 && datagram[1] == 0x01
                && datagram[4] == 0x21 && datagram[5] == 0x12
                && datagram[6] == 0xA4 && datagram[7] == 0x42) {
                m_sawBinding = true;
            }
        }
        return m_sawBinding;
    }

private:
    QString m_address;
    int m_fd = -1;
    int m_port = 0;
    bool m_sawBinding = false;
};

// Long enough for any retirement: the bound plus a stopping webrtcbin.
constexpr int kRetiredWithinMs =
    lightning::webrtc::Retirer::kGatheringBoundMs + 3000;

/// A camera shaped like v4l2src on a raw-only (YUYV) device: before it is
/// opened it offers its template, which lists image/jpeg as v4l2src's does
/// for every camera, and once opened (READY) it offers only what the device
/// has. A videotestsrc cannot stand in: its template has no jpeg, so the MJPG
/// description already fails to parse and the old fallback catches it.
struct RawOnlyCam {
    GstBin parent;
};
struct RawOnlyCamClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(RawOnlyCam, raw_only_cam, GST_TYPE_BIN)

gboolean rawOnlyCamQuery(GstPad *pad, GstObject *parent, GstQuery *query)
{
    if (GST_QUERY_TYPE(query) == GST_QUERY_CAPS
        && GST_STATE(GST_ELEMENT(parent)) < GST_STATE_READY) {
        GstCaps *filter = nullptr;
        gst_query_parse_caps(query, &filter);
        GstCaps *templ = gst_caps_from_string(
            "image/jpeg; video/x-raw, format=(string)YUY2");
        if (filter) {
            GstCaps *both = gst_caps_intersect(templ, filter);
            gst_caps_unref(templ);
            templ = both;
        }
        gst_query_set_caps_result(query, templ);
        gst_caps_unref(templ);
        return TRUE;
    }
    return gst_pad_query_default(pad, parent, query);
}

void raw_only_cam_init(RawOnlyCam *self)
{
    GstElement *device = gst_parse_bin_from_description(
        "videotestsrc is-live=true "
        "! capsfilter caps=\"video/x-raw,format=YUY2,width=640,height=480,"
        "framerate=30/1,pixel-aspect-ratio=1/1\"",
        TRUE, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    GstPad *ghost = gst_ghost_pad_new("src", inner);
    gst_object_unref(inner);
    gst_pad_set_query_function(ghost, rawOnlyCamQuery);
    gst_element_add_pad(GST_ELEMENT(self), ghost);
}

void raw_only_cam_class_init(RawOnlyCamClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Raw-only camera (test)", "Source/Video",
        "A v4l2src-shaped YUYV-only camera", "Lightning");
}

/// A camera whose start blocks the thread that starts it, as pipewiresrc's
/// does (`wait_started`) until PipeWire has negotiated a format with the
/// camera node: up to 30 s when the host cannot stream that node.
struct SlowStartCam {
    GstBin parent;
};
struct SlowStartCamClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowStartCam, slow_start_cam, GST_TYPE_BIN)

constexpr int kSlowStartCamMs = 2500;

GstStateChangeReturn slowStartCamChangeState(GstElement *element,
                                             GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED)
        g_usleep(gulong(kSlowStartCamMs) * 1000);
    return GST_ELEMENT_CLASS(slow_start_cam_parent_class)
        ->change_state(element, transition);
}

void slow_start_cam_init(SlowStartCam *self)
{
    GstElement *device =
        gst_element_factory_make("videotestsrc", "slowcamdevice");
    g_object_set(device, "is-live", TRUE, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    GstPad *ghost = gst_ghost_pad_new("src", inner);
    gst_object_unref(inner);
    gst_element_add_pad(GST_ELEMENT(self), ghost);
}

void slow_start_cam_class_init(SlowStartCamClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-start camera (test)", "Source/Video",
        "A pipewiresrc-shaped camera whose start blocks", "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowStartCamChangeState;
}

/// A camera that starts at once and then blocks the thread that STOPS it, as
/// a pipewiresrc whose PipeWire link never negotiated does in every later
/// state change (pw_thread_loop_timed_wait, 30 s; measured on Debian 12).
struct SlowStopCam {
    GstBin parent;
};
struct SlowStopCamClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowStopCam, slow_stop_cam, GST_TYPE_BIN)

GstStateChangeReturn slowStopCamChangeState(GstElement *element,
                                            GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED
        || transition == GST_STATE_CHANGE_PAUSED_TO_READY)
        g_usleep(gulong(kSlowStartCamMs) * 1000);
    return GST_ELEMENT_CLASS(slow_stop_cam_parent_class)
        ->change_state(element, transition);
}

void slow_stop_cam_init(SlowStopCam *self)
{
    GstElement *device =
        gst_element_factory_make("videotestsrc", "slowstopdevice");
    g_object_set(device, "is-live", TRUE, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    GstPad *ghost = gst_ghost_pad_new("src", inner);
    gst_object_unref(inner);
    gst_element_add_pad(GST_ELEMENT(self), ghost);
}

void slow_stop_cam_class_init(SlowStopCamClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-stop camera (test)", "Source/Video",
        "A wedged-pipewiresrc-shaped camera whose stop blocks", "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowStopCamChangeState;
}

/// The loudest sample (in thousandths of full scale) of the decoded audio
/// crossing a pad, and how many buffers did.
struct PeakMeter {
    std::atomic<int> peakMilli{0};
    std::atomic<int> buffers{0};
};

GstPadProbeReturn meterPeak(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    auto *meter =
        static_cast<std::shared_ptr<PeakMeter> *>(data)->get();
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!buffer || !caps) {
        if (caps)
            gst_caps_unref(caps);
        return GST_PAD_PROBE_OK;
    }
    const gchar *format =
        gst_structure_get_string(gst_caps_get_structure(caps, 0), "format");
    GstMapInfo map;
    if (format && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        double peak = 0;
        if (g_strcmp0(format, "S16LE") == 0) {
            const auto *samples = reinterpret_cast<const gint16 *>(map.data);
            for (gsize i = 0; i < map.size / 2; ++i)
                peak = qMax(peak, qAbs(double(samples[i])) / 32768.0);
        } else if (g_strcmp0(format, "F32LE") == 0) {
            const auto *samples = reinterpret_cast<const float *>(map.data);
            for (gsize i = 0; i < map.size / 4; ++i)
                peak = qMax(peak, qAbs(double(samples[i])));
        }
        gst_buffer_unmap(buffer, &map);
        const int milli = int(peak * 1000.0);
        int seen = meter->peakMilli.load();
        while (milli > seen
               && !meter->peakMilli.compare_exchange_weak(seen, milli)) {
        }
        meter->buffers.fetch_add(1);
    }
    gst_caps_unref(caps);
    return GST_PAD_PROBE_OK;
}

void meterPeaksAt(GstPad *pad, const std::shared_ptr<PeakMeter> &meter)
{
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, meterPeak,
                      new std::shared_ptr<PeakMeter>(meter), [](gpointer data) {
                          delete static_cast<std::shared_ptr<PeakMeter> *>(
                              data);
                      });
}

/// The first element inside `bin` whose name starts with `prefix`, with a
/// ref, or null.
GstElement *elementWithPrefix(GstElement *bin, const char *prefix)
{
    if (!bin)
        return nullptr;
    GstElement *found = nullptr;
    GstIterator *it = gst_bin_iterate_recurse(GST_BIN(bin));
    GValue item = G_VALUE_INIT;
    while (!found && gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        auto *element = GST_ELEMENT(g_value_get_object(&item));
        if (g_str_has_prefix(GST_OBJECT_NAME(element), prefix))
            found = GST_ELEMENT(gst_object_ref(element));
        g_value_reset(&item);
    }
    if (G_IS_VALUE(&item))
        g_value_unset(&item);
    gst_iterator_free(it);
    return found;
}

/// The current caps on `pad` of the element `name` inside `bin`, as text.
QString padCapsOf(GstElement *bin, const char *name, const char *pad)
{
    QString text;
    if (GstElement *element = gst_bin_get_by_name(GST_BIN(bin), name)) {
        if (GstPad *p = gst_element_get_static_pad(element, pad)) {
            if (GstCaps *caps = gst_pad_get_current_caps(p)) {
                gchar *s = gst_caps_to_string(caps);
                text = QString::fromUtf8(s);
                g_free(s);
                gst_caps_unref(caps);
            }
            gst_object_unref(p);
        }
        gst_object_unref(element);
    }
    return text;
}

/// Wires a sender's publisher to a receiver's subscriber, the way the SFU
/// relays them, and records the first failure either reports.
void wireLoopback(SfuMediaEngine &sender, SfuMediaEngine &receiver,
                  QObject *context, QString *failure)
{
    const auto note = [failure](const QString &why) {
        if (failure->isEmpty())
            *failure = why;
    };
    QObject::connect(&sender, &SfuMediaEngine::failed, context, note);
    QObject::connect(&receiver, &SfuMediaEngine::failed, context, note);
    QObject::connect(
        &sender, &SfuMediaEngine::localDescription, &receiver,
        [&receiver](int target, const QString &kind, const QString &sdp) {
            if (target == int(SfuMediaEngine::Target::Publisher)
                && kind == QStringLiteral("offer")) {
                receiver.applyRemoteDescription(
                    SfuMediaEngine::Target::Subscriber, kind, sdp);
            }
        });
    QObject::connect(
        &receiver, &SfuMediaEngine::localDescription, &sender,
        [&sender](int target, const QString &kind, const QString &sdp) {
            if (target == int(SfuMediaEngine::Target::Subscriber)
                && kind == QStringLiteral("answer")) {
                sender.applyRemoteDescription(
                    SfuMediaEngine::Target::Publisher, kind, sdp);
            }
        });
    QObject::connect(&sender, &SfuMediaEngine::localCandidate, &receiver,
                     [&receiver](int target, const QString &init) {
                         if (target == int(SfuMediaEngine::Target::Publisher))
                             receiver.applyRemoteCandidate(
                                 SfuMediaEngine::Target::Subscriber, init);
                     });
    QObject::connect(&receiver, &SfuMediaEngine::localCandidate, &sender,
                     [&sender](int target, const QString &init) {
                         if (target == int(SfuMediaEngine::Target::Subscriber))
                             sender.applyRemoteCandidate(
                                 SfuMediaEngine::Target::Publisher, init);
                     });
}

/// The first sink element inside `bin` (borrowed), or null.
GstElement *firstSinkOf(GstElement *bin)
{
    if (!bin)
        return nullptr;
    GstIterator *it = gst_bin_iterate_sinks(GST_BIN(bin));
    GValue item = G_VALUE_INIT;
    GstElement *found = nullptr;
    if (gst_iterator_next(it, &item) == GST_ITERATOR_OK)
        found = GST_ELEMENT(g_value_get_object(&item));
    g_value_unset(&item);
    gst_iterator_free(it);
    return found;
}

/// Counts the buffers reaching `sink`. The counter must outlive the probe.
void countBuffersAt(GstElement *sink, std::atomic<int> *counter)
{
    GstPad *pad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer data) {
            static_cast<std::atomic<int> *>(data)->fetch_add(1);
            return GST_PAD_PROBE_OK;
        },
        counter, nullptr);
    gst_object_unref(pad);
}

/// Makes `sink` fail the way pulsesink does when the sound server drops the
/// client: it posts an error and returns GST_FLOW_ERROR for every buffer.
void failLikeADisconnectedSoundServer(GstElement *sink)
{
    GstPad *pad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *probed, GstPadProbeInfo *info, gpointer) {
            GstElement *owner = gst_pad_get_parent_element(probed);
            if (owner) {
                GError *error = g_error_new_literal(
                    GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
                    "pa_stream_writable_size() failed: Connection terminated");
                gst_element_post_message(
                    owner, gst_message_new_error(GST_OBJECT(owner), error,
                                                 "test"));
                g_error_free(error);
                gst_object_unref(owner);
            }
            gst_buffer_unref(GST_PAD_PROBE_INFO_BUFFER(info));
            GST_PAD_PROBE_INFO_DATA(info) = nullptr;
            GST_PAD_PROBE_INFO_FLOW_RETURN(info) = GST_FLOW_ERROR;
            return GST_PAD_PROBE_HANDLED;
        },
        nullptr, nullptr);
    gst_object_unref(pad);
}
} // namespace

/// Collects Qt log output for the duration of one test. The diagnostics under
/// test are log lines, so asserting on them asserts the feature.
class LogCapture
{
public:
    LogCapture()
    {
        m_outer = instance();
        instance() = this;
        m_previous = qInstallMessageHandler(&LogCapture::handler);
    }
    ~LogCapture()
    {
        qInstallMessageHandler(m_previous);
        instance() = m_outer;
    }
    void clear()
    {
        QMutexLocker lock(&m_mutex);
        m_lines.clear();
    }
    bool contains(const char *needle) const
    {
        return text().contains(QLatin1String(needle));
    }
    int count(const char *needle) const
    {
        QMutexLocker lock(&m_mutex);
        int n = 0;
        for (const QString &line : m_lines) {
            if (line.contains(QLatin1String(needle)))
                ++n;
        }
        return n;
    }
    QString text() const
    {
        QMutexLocker lock(&m_mutex);
        return m_lines.join(QLatin1Char('\n'));
    }

private:
    static LogCapture *&instance()
    {
        static LogCapture *self = nullptr;
        return self;
    }
    static void handler(QtMsgType type, const QMessageLogContext &context,
                        const QString &message)
    {
        // Locked: pad probes log from GStreamer streaming threads.
        if (LogCapture *self = instance()) {
            QMutexLocker lock(&self->m_mutex);
            self->m_lines.append(message);
        }
        Q_UNUSED(type);
        Q_UNUSED(context);
    }
    mutable QMutex m_mutex;
    QStringList m_lines;
    QtMessageHandler m_previous = nullptr;
    LogCapture *m_outer = nullptr;
};

/// The real decrypt probe fed hand-built frames: appsrc -> fakesink with the
/// production receive probe installed on appsrc's src pad. Server-injected
/// frames only come from an SFU, so a two-engine loopback cannot produce them.
class DecryptProbeRig
{
public:
    DecryptProbeRig(SfuMediaEngine &engine, const QString &streamId,
                    bool video = false)
    {
        m_pipeline = gst_pipeline_new(nullptr);
        m_src = gst_element_factory_make("appsrc", nullptr);
        GstElement *sink = gst_element_factory_make("fakesink", nullptr);
        if (!m_pipeline || !m_src || !sink)
            return;
        g_object_set(m_src, "format", GST_FORMAT_TIME, nullptr);
        g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
        gst_bin_add_many(GST_BIN(m_pipeline), m_src, sink, nullptr);
        if (!gst_element_link(m_src, sink))
            return;
        GstPad *srcPad = gst_element_get_static_pad(m_src, "src");
        engine.installDecryptProbeForTest(srcPad, video, streamId);
        // Counts what the probe let through.
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        m_passed = new std::atomic<int>(0);
        gst_pad_add_probe(
            sinkPad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            },
            m_passed,
            [](gpointer data) { delete static_cast<std::atomic<int> *>(data); });
        m_passedView = m_passed;
        gst_object_unref(sinkPad);
        gst_object_unref(srcPad);
        m_ok = gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
            != GST_STATE_CHANGE_FAILURE;
    }
    ~DecryptProbeRig() { finish(); }

    bool ok() const { return m_ok; }
    void push(const QByteArray &frame)
    {
        GstBuffer *buffer = gst_buffer_new_allocate(
            nullptr, static_cast<gsize>(frame.size()), nullptr);
        gst_buffer_fill(buffer, 0, frame.constData(),
                        static_cast<gsize>(frame.size()));
        GST_BUFFER_PTS(buffer) = m_pts;
        GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
        m_pts += 20 * GST_MSECOND;
        gst_app_src_push_buffer(GST_APP_SRC(m_src), buffer); // takes it
    }
    /// Push EOS and wait for it at the sink, so every earlier frame has been
    /// through the probe.
    bool drain()
    {
        gst_app_src_end_of_stream(GST_APP_SRC(m_src));
        GstBus *bus = gst_element_get_bus(m_pipeline);
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        const bool eos = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
        if (msg)
            gst_message_unref(msg);
        gst_object_unref(bus);
        return eos;
    }
    int passed() const { return m_passedView ? m_passedView->load() : -1; }
    /// Tear down, which frees the probe context (and logs its summary).
    void finish()
    {
        if (!m_pipeline)
            return;
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        m_passedView = nullptr;
    }

private:
    GstElement *m_pipeline = nullptr;
    GstElement *m_src = nullptr;
    std::atomic<int> *m_passed = nullptr;       // owned by the probe
    std::atomic<int> *m_passedView = nullptr;   // null once torn down
    GstClockTime m_pts = 0;
    bool m_ok = false;
};

/// livekit-server's `OpusSilenceFrame` (pkg/sfu/downtrack.go): f8 ff fe and
/// 77 zeros, injected into an audio track on mute.
QByteArray opusSilenceFrame()
{
    QByteArray silence(80, '\0');
    silence[0] = char(0xf8);
    silence[1] = char(0xff);
    silence[2] = char(0xfe);
    return silence;
}

/// A livekit-shaped room trailer (base62, 43 bytes) whose last byte is 'R'
/// (key index 82).
QByteArray roomTrailer()
{
    return QByteArray("k3P9dQ2mZ7xW4vB8nT1cY6hJ0fL5sG2aE9rU3oKqXiR");
}

/// A scripted per-application listing for the engine's share-audio seam.
struct ScriptedListing {
    QList<lightning::shareaudio::Stream> live;
    QSet<QString> running;   // ids whose process runs (Windows policy)
};

static lightning::shareaudio::Stream scriptedStream(const QString &serial,
                                                    const QString &key,
                                                    qint64 start = 0)
{
    lightning::shareaudio::Stream s;
    s.serial = serial;
    s.appKey = key;
    s.appName = key;
    s.pid = serial.toLongLong();
    s.startTime = start;
    return s;
}

static void installListing(SfuMediaEngine &engine,
                           const std::shared_ptr<ScriptedListing> &listing,
                           bool followsProcess)
{
    lightning::shareaudio::BranchOptions options;
    options.followsProcess = followsProcess;
    options.retireOnDisconnect = false;
    engine.setShareAudioSourceForTest(
        [listing](const QList<lightning::shareaudio::Stream> &check) {
            lightning::shareaudio::Enumeration e;
            e.streams = listing->live;
            for (const lightning::shareaudio::Stream &c : check) {
                if (listing->running.contains(c.id()))
                    e.running.insert(c.id());
            }
            return e;
        },
        options);
}

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

    // Review M2: a device binding was set with g_object_set(char*), and
    // avfvideosrc `device-index` / osxaudiosrc `device` are ints, which is
    // undefined behaviour. The value is now converted to the property's type,
    // and one it refuses is reported as not set (a camera then opens nothing).
    // Measured on core elements, which every build has.
    void aDeviceBindingIsConvertedToThePropertysType()
    {
        // An int property, as avfvideosrc's device-index.
        QCOMPARE(SfuMediaEngine::applyDevicePropertyForTest(
                     QStringLiteral("fakesrc"), QStringLiteral("num-buffers"),
                     QStringLiteral("7")),
                 QStringLiteral("7"));
        // Not a number: refused, not set to garbage.
        QVERIFY(SfuMediaEngine::applyDevicePropertyForTest(
                    QStringLiteral("fakesrc"), QStringLiteral("num-buffers"),
                    QStringLiteral("banana"))
                    .isEmpty());
        // Out of the property's range (num-buffers is -1..G_MAXINT).
        QVERIFY(SfuMediaEngine::applyDevicePropertyForTest(
                    QStringLiteral("fakesrc"), QStringLiteral("num-buffers"),
                    QStringLiteral("-5"))
                    .isEmpty());
        // A string property, as v4l2src's device, is set as it is.
        QCOMPARE(SfuMediaEngine::applyDevicePropertyForTest(
                     QStringLiteral("filesrc"), QStringLiteral("location"),
                     QStringLiteral("/dev/video61")),
                 QStringLiteral("/dev/video61"));
        // An unknown property sets nothing.
        QVERIFY(SfuMediaEngine::applyDevicePropertyForTest(
                    QStringLiteral("fakesrc"), QStringLiteral("device-path"),
                    QStringLiteral("x"))
                    .isEmpty());
    }

    // LIGHTNING_CALL_STATS_TRACE: unset/0/off = no trace; 1/true/yes = 5 s;
    // a number = that many seconds, bounded.
    void statsTraceIntervalParsesItsEnvironment()
    {
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QString()), 0);
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QStringLiteral("0")), 0);
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QStringLiteral("off")), 0);
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QStringLiteral("1")), 5000);
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QStringLiteral("true")), 5000);
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QStringLiteral("10")), 10000);
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QStringLiteral("abc")), 5000);
        QCOMPARE(SfuMediaEngine::statsTraceIntervalMs(QStringLiteral("9999")), 600000);
    }

    // A capture of a dead device must be reported: opusenc turns silence into
    // real frames, so encrypt/transport counters look healthy either way.
    void aSustainedlySilentCaptureIsReported()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        QVERIFY(spy.isValid());

        // Digital silence, reported every 200 ms as the element does.
        for (qint64 t = 0; t < SfuMediaEngine::kMicSilenceWindowMs; t += 200)
            engine.handleMicLevelAt(-350.0, t);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());

        // The window closes exactly on the boundary.
        engine.handleMicLevelAt(-350.0, SfuMediaEngine::kMicSilenceWindowMs);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);
        QVERIFY(engine.microphoneSilentForTest());
    }

    // A pause between sentences is not a broken microphone.
    void aQuietMomentIsNotASilentMicrophone()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        for (qint64 t = 0; t < SfuMediaEngine::kMicSilenceWindowMs * 3;
             t += 200) {
            // Nothing for most of the window, then one word.
            const bool speaks = (t / 200) % 40 == 0;
            engine.handleMicLevelAt(speaks ? -18.0 : -350.0, t);
        }
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());
    }

    // The notice clears when the device comes back.
    void aMicrophoneThatComesBackClearsTheNotice()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        for (qint64 t = 0; t <= SfuMediaEngine::kMicSilenceWindowMs; t += 200)
            engine.handleMicLevelAt(-350.0, t);
        QCOMPARE(spy.count(), 1);
        engine.handleMicLevelAt(-12.0,
                                SfuMediaEngine::kMicSilenceWindowMs + 200);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.takeLast().at(0).toBool(), false);
        QVERIFY(!engine.microphoneSilentForTest());
    }

    // Mute is requested silence and is never reported.
    void aMutedMicrophoneIsNeverReportedAsSilent()
    {
        SfuMediaEngine engine;
        engine.setMicrophoneMuted(true);
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        for (qint64 t = 0; t < SfuMediaEngine::kMicSilenceWindowMs * 2;
             t += 200) {
            engine.handleMicLevelAt(-350.0, t);
        }
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());
    }

    // Muting clears a raised notice at once: `level` sits after the valve, so
    // a muted capture posts no further levels.
    void mutingClearsARaisedSilenceNoticeWithoutAnotherLevelReport()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        for (qint64 t = 0; t <= SfuMediaEngine::kMicSilenceWindowMs; t += 200)
            engine.handleMicLevelAt(-350.0, t);
        QVERIFY(engine.microphoneSilentForTest());
        QCOMPARE(spy.count(), 1);

        engine.setMicrophoneMuted(true);
        QVERIFY(!engine.microphoneSilentForTest());
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.takeLast().at(0).toBool(), false);

        // Unmuting starts a fresh window.
        engine.setMicrophoneMuted(false);
        engine.handleMicLevelAt(-350.0,
                                SfuMediaEngine::kMicSilenceWindowMs + 5000);
        QVERIFY(!engine.microphoneSilentForTest());
        QCOMPARE(spy.count(), 1);
    }

    // 2026-10-04 call log: peaks of -60..-78 dBFS between sentences raised
    // "THE MICROPHONE IS CAPTURING NOTHING" four times. Fails on the old
    // -60 dBFS / 10 s rule (fires after 10 s), passes with the dead ceiling.
    void aQuietRoomIsNotADeadMicrophone()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        const double peaks[] = {-62.0, -78.0, -70.0, -66.0, -75.0};
        int i = 0;
        for (qint64 t = 0; t < SfuMediaEngine::kMicQuietWindowMs - 200;
             t += 200)
            engine.handleMicLevelAt(peaks[i++ % 5], t);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());
    }

    // A device that stays that quiet for a full minute is still worth saying
    // so; one louder report restarts the long window.
    void aMinuteOfVeryQuietCaptureIsLoggedButNeverRaisesTheBadge()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        for (qint64 t = 0; t <= SfuMediaEngine::kMicQuietWindowMs; t += 200)
            engine.handleMicLevelAt(-70.0, t);
        // The badge says nobody can hear you; a live quiet mic is not that.
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());
        QVERIFY(engine.microphoneQuietForTest());
        engine.handleMicLevelAt(-20.0, SfuMediaEngine::kMicQuietWindowMs + 200);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneQuietForTest());
    }

    // The silence threshold, pinned as a boundary.
    void theSilenceCeilingIsAPeakNoSpeechStaysUnder()
    {
        // -85 dBFS is the dead-device ceiling: a live room peaks -60..-78.
        // `level` reports digital silence as -350 (its floor), not a sentinel.
        QCOMPARE(SfuMediaEngine::micSilenceSince(-84.0, -1, 1000), qint64(-1));
        QCOMPARE(SfuMediaEngine::micSilenceSince(-86.0, -1, 1000), qint64(1000));
        // The mark is carried, not restamped, or the window could never close.
        QCOMPARE(SfuMediaEngine::micSilenceSince(-86.0, 1000, 5000),
                 qint64(1000));
        // One audible report clears it.
        QCOMPARE(SfuMediaEngine::micSilenceSince(-20.0, 1000, 5000),
                 qint64(-1));
        // t=0 is a legal instant and must not read as "no mark".
        QCOMPARE(SfuMediaEngine::micSilenceSince(-350.0, -1, 0), qint64(0));
        QVERIFY(SfuMediaEngine::micSilenceReached(
            0, SfuMediaEngine::kMicSilenceWindowMs));
        QVERIFY(!SfuMediaEngine::micSilenceReached(-1, 999999));
        QVERIFY(!SfuMediaEngine::micSilenceReached(
            1000, 1000 + SfuMediaEngine::kMicSilenceWindowMs - 1));
        QVERIFY(SfuMediaEngine::micSilenceReached(
            1000, 1000 + SfuMediaEngine::kMicSilenceWindowMs));
    }

    // The level meter is in the bin the engine actually published, not in a
    // pipeline the test composed.
    void theCaptureChainMeasuresItsOwnLevel()
    {
        GstElementFactory *factory = gst_element_factory_find("level");
        if (!factory)
            QSKIP("gst-plugins-good's `level` is absent from this build");
        gst_object_unref(factory);

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        QCOMPARE(failed.count(), 0);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-audio")));
        QVERIFY(engine.publishedBinHasElementForTest(
            QStringLiteral("cid-audio"), QStringLiteral("miclevel")));
        // The encoder is downstream: the level is what the far end receives.
        QVERIFY(engine.publishedBinHasElementForTest(
            QStringLiteral("cid-audio"), QStringLiteral("audioenc")));
        engine.stop();
    }

    // The in-call meter redraws from the call's own `level`, so the element
    // must report at the meter's rate: at the old 200 ms the bar moved five
    // times a second. Fails on the old interval.
    void theCaptureMeterReportsAtTheMetersRate()
    {
        GstElementFactory *factory = gst_element_factory_find("level");
        if (!factory)
            QSKIP("gst-plugins-good's `level` is absent from this build");
        gst_object_unref(factory);
        QCOMPARE(SfuMediaEngine::kMicLevelIntervalNs, quint64(50000000));

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        const QString built = engine.lastAudioDescriptionForTest();
        QVERIFY2(built.contains(QStringLiteral("level name=miclevel post-messages="
                                               "true interval=50000000")),
                 qPrintable(built));
        // The front the Settings microphone test reuses is the one the call
        // publishes, verbatim.
        SfuMediaEngine::MicrophoneCapture capture;
        capture.source = QStringLiteral(
            "audiotestsrc is-live=true wave=sine freq=440 volume=0.05 "
            "name=micsrc");
        QVERIFY2(built.startsWith(SfuMediaEngine::microphoneFrontDescription(capture)),
                 qPrintable(built));
        engine.stop();
    }

    // Every unmuted reading reaches the in-call meter; one in flight when
    // the user muted does not.
    void everyUnmutedLevelReachesTheMeter()
    {
        SfuMediaEngine engine;
        QSignalSpy levels(&engine, &SfuMediaEngine::localAudioLevel);
        QVERIFY(levels.isValid());
        // The meter reads what is SENT (`miclevel`, after suppression).
        engine.handleMeterLevel(-20.0);
        engine.handleMeterLevel(-350.0);
        QCOMPARE(levels.count(), 2);
        QCOMPARE(levels.at(0).at(0).toDouble(), -20.0);
        // Digital silence is a reading like any other.
        QCOMPARE(levels.at(1).at(0).toDouble(), -350.0);
        engine.setMicrophoneMuted(true);
        engine.handleMeterLevel(-20.0);
        QCOMPARE(levels.count(), 2);
        // The captured level never moves the meter.
        engine.setMicrophoneMuted(false);
        engine.handleMicLevelAt(-20.0, 100);
        QCOMPARE(levels.count(), 2);
    }

    // The peak reader both the call and the Settings test use: the loudest
    // channel, and -350 (digital silence) as a reading rather than a failure.
    void theLevelReaderTakesTheLoudestChannelAndKeepsSilence()
    {
        const auto levelWith = [](std::initializer_list<double> peaks) {
            GstStructure *fields = gst_structure_new_empty("level");
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            GValueArray *array = g_value_array_new(peaks.size());
            GValue one = G_VALUE_INIT;
            g_value_init(&one, G_TYPE_DOUBLE);
            for (const double peak : peaks) {
                g_value_set_double(&one, peak);
                g_value_array_append(array, &one);
            }
            g_value_unset(&one);
            GValue holder = G_VALUE_INIT;
            g_value_init(&holder, G_TYPE_VALUE_ARRAY);
            g_value_take_boxed(&holder, array);
            G_GNUC_END_IGNORE_DEPRECATIONS
            gst_structure_take_value(fields, "peak", &holder);
            return fields;
        };
        double peak = 0.0;
        GstStructure *stereo = levelWith({-30.0, -12.0});
        QVERIFY(SfuMediaEngine::readLevelPeak(stereo, &peak));
        QCOMPARE(peak, -12.0);
        gst_structure_free(stereo);

        GstStructure *silent = levelWith({-350.0});
        QVERIFY(SfuMediaEngine::readLevelPeak(silent, &peak));
        QCOMPARE(peak, -350.0);
        gst_structure_free(silent);

        // No peak at all is unreadable, not silence.
        GstStructure *empty = gst_structure_new_empty("level");
        peak = 1.0;
        QVERIFY(!SfuMediaEngine::readLevelPeak(empty, &peak));
        QCOMPARE(peak, 1.0);
        gst_structure_free(empty);
    }

    // The multi-input stages reach the description publishAudio() actually
    // hands GStreamer, and GStreamer accepts it.
    void theCaptureChainKeepsItsOrdinaryShapeForAnOrdinaryMicrophone()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        QCOMPARE(failed.count(), 0);
        const QString built = engine.lastAudioDescriptionForTest();
        QVERIFY(!built.isEmpty());
        // No device, so no multi-input stages.
        QVERIFY2(!built.contains(QStringLiteral("mix-matrix")),
                 qPrintable(built.left(200)));
        QVERIFY2(!built.contains(QStringLiteral("channel-mask")),
                 qPrintable(built.left(200)));
        QVERIFY(built.contains(QStringLiteral("audio/x-raw,channels=1")));
        // The capture queue is bounded and leaky; a default queue holds 1 s.
        QVERIFY2(built.contains(QStringLiteral("leaky=downstream")),
                 qPrintable(built.left(300)));
        QVERIFY2(built.contains(QStringLiteral("max-size-time=100000000")),
                 qPrintable(built.left(300)));
        engine.stop();
    }

    // GitHub #20: the microphone the engine publishes runs exactly the
    // selected noise suppressor — read from the description it handed
    // GStreamer AND from the elements inside the bin it built. Before the
    // selector, webrtcdsp suppressed in every chain and nothing else existed.
    // GitHub #20, live finding 2026-10-06: with DeepFilterNet, a microphone
    // carrying -29 dBFS of fan noise and no speech read as "CAPTURING
    // NOTHING" after 10 s, because the dead-microphone judgement read the
    // level AFTER the suppressor. It reads `miccapturelevel` now, which sits
    // after the valve and BEFORE the suppressor; `miclevel` (the meter, and
    // the RFC 6464 speaking level) stays after it, measuring what is sent.
    void theDeadMicrophoneJudgementMeasuresBeforeTheSuppressor()
    {
        GstElementFactory *factory = gst_element_factory_find("level");
        if (!factory)
            QSKIP("gst-plugins-good's `level` is absent from this build");
        gst_object_unref(factory);
        const QString cid = QStringLiteral("cid-audio");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setNoiseSuppressionMode(calls::noise::Mode::DeepFilterNet);
        engine.start();
        engine.publishAudio(cid);
        const QString built = engine.lastAudioDescriptionForTest();
        QVERIFY2(engine.publishedBinHasElementForTest(
                     cid, QStringLiteral("miccapturelevel")),
                 qPrintable(built));
        const qsizetype valve = built.indexOf(QStringLiteral("name=micvalve"));
        const qsizetype captured =
            built.indexOf(QStringLiteral("name=miccapturelevel"));
        const qsizetype denoise = built.indexOf(QStringLiteral("name=micdenoise"));
        const qsizetype sent = built.indexOf(QStringLiteral("name=miclevel "));
        QVERIFY2(valve >= 0 && captured >= 0 && denoise >= 0 && sent >= 0,
                 qPrintable(built));
        // Muted posts nothing; then before the suppressor; the meter after.
        QVERIFY(valve < captured);
        QVERIFY(captured < denoise);
        QVERIFY(denoise < sent);
        engine.stop();
    }

    // The two readings feed different things: the captured level alone
    // decides "dead microphone", the sent level alone moves the meter.
    void aMicrophoneTheSuppressorCleansIsNotADeadOne()
    {
        SfuMediaEngine engine;
        QSignalSpy silent(&engine, &SfuMediaEngine::localAudioSilent);
        QSignalSpy meter(&engine, &SfuMediaEngine::localAudioLevel);
        // A fan at -29 dBFS, cleaned to digital silence: well past the window.
        for (qint64 t = 0; t <= SfuMediaEngine::kMicSilenceWindowMs * 2;
             t += 200) {
            engine.handleMicLevelAt(-29.0, t);
            engine.handleMeterLevel(-350.0);
        }
        QVERIFY(!engine.microphoneSilentForTest());
        QCOMPARE(silent.count(), 0);
        // Only the sent readings reached the meter, -350 included.
        QCOMPARE(meter.count(),
                 int(SfuMediaEngine::kMicSilenceWindowMs * 2 / 200 + 1));
        QCOMPARE(meter.last().at(0).toDouble(), -350.0);

        // A really dead device is still reported, whatever the meter says;
        // -350 is `level`'s floor for digital silence and counts as silence.
        const qint64 start = SfuMediaEngine::kMicSilenceWindowMs * 3;
        for (qint64 t = start;
             t <= start + SfuMediaEngine::kMicSilenceWindowMs; t += 200) {
            engine.handleMicLevelAt(-350.0, t);
            engine.handleMeterLevel(-20.0);
        }
        QVERIFY(engine.microphoneSilentForTest());
        QCOMPARE(silent.count(), 1);
        QVERIFY(silent.last().at(0).toBool());
    }

    // Live finding 2026-10-06: RSS grew 11-19 MB per leave/rejoin with
    // DeepFilterNet. Whatever the allocator does, the objects must go: after
    // N calls of the REAL publish chain in a neural mode, no backend and no
    // `lightningdenoise` element may be alive. A counting wrapper around the
    // real factory (or a fake where the backend is not built in) counts every
    // backend the element ever made.
    void repeatedCallsWithANeuralSuppressorLeaveNothingAlive()
    {
        struct Counting final : calls::noise::NoiseSuppressor {
            explicit Counting(std::unique_ptr<calls::noise::NoiseSuppressor> real,
                              calls::noise::Mode mode)
                : m_real(std::move(real)), m_mode(mode)
            {
                ++alive();
            }
            ~Counting() override { --alive(); }
            static std::atomic<int> &alive()
            {
                static std::atomic<int> count{0};
                return count;
            }
            calls::noise::Mode mode() const override { return m_mode; }
            int frameSize() const override { return calls::noise::kFrameSamples; }
            bool ok() const override { return true; }
            void process(float *frame) noexcept override
            {
                if (m_real)
                    m_real->process(frame);
            }
            int latencySamples() const override
            {
                return m_real ? m_real->latencySamples() : 0;
            }
            void reset() noexcept override
            {
                if (m_real)
                    m_real->reset();
            }
            std::unique_ptr<calls::noise::NoiseSuppressor> m_real;
            calls::noise::Mode m_mode;
        };
        const calls::noise::Mode mode = calls::noise::Mode::DeepFilterNet;
        calls::noise::setSuppressorFactoryForTest(
            [](calls::noise::Mode m)
                -> std::unique_ptr<calls::noise::NoiseSuppressor> {
                // The real backend when built in, so its handle is created
                // and destroyed for real; otherwise the wrapper alone.
                return std::make_unique<Counting>(
                    calls::noise::createSuppressor(m), m);
            });
        // Earlier cases' pipelines may still be on their way out (the
        // webrtcbin retirer, then finalization on a pool thread); under a
        // loaded parallel run that is seconds. Start from none at all rather
        // than from a snapshot that is still shrinking.
        QTRY_COMPARE_WITH_TIMEOUT(calls::noise::liveDenoiseElements(), 0, 30000);
        const QString cid = QStringLiteral("cid-audio");
        {
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.setNoiseSuppressionMode(mode);
            for (int call = 0; call < 5; ++call) {
                engine.start();
                engine.publishAudio(cid);
                QVERIFY(engine.publishedBinHasElementForTest(
                    cid, QStringLiteral("micdenoise")));
                // The backend is built on a pool thread: let it exist (a
                // real DeepFilterNet unpacks its model, slowly under load).
                QTRY_VERIFY_WITH_TIMEOUT(Counting::alive().load() >= 1, 30000);
                // A live switch mid-call retires one backend and builds another.
                if (call % 2 == 1) {
                    engine.setNoiseSuppressionMode(calls::noise::Mode::RNNoise);
                    engine.setNoiseSuppressionMode(mode);
                }
                engine.stop();
            }
            // Teardown's own end: the retirer has let every pipeline go
            // (bounded at kGatheringBoundMs), then elements are finalized and
            // their backends destroyed, possibly on pool threads.
            QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0, 30000);
            QTRY_COMPARE_WITH_TIMEOUT(calls::noise::liveDenoiseElements(), 0,
                                      30000);
            QTRY_COMPARE_WITH_TIMEOUT(Counting::alive().load(), 0, 30000);
        }
        calls::noise::setSuppressorFactoryForTest({});
        // Nothing is created after the engine is gone.
        QCOMPARE(calls::noise::liveDenoiseElements(), 0);
        QCOMPARE(Counting::alive().load(), 0);
    }

    // Live finding 2026-10-06: WebRTC -> DeepFilterNet in a call left about a
    // second unsuppressed (webrtcdsp off at once, the model still building).
    // The engine keeps webrtcdsp suppressing until the denoiser reports.
    void aSwitchFromWebrtcKeepsSuppressingUntilTheNewBackendRuns()
    {
        GstElementFactory *factory = gst_element_factory_find("webrtcdsp");
        if (!factory)
            QSKIP("webrtcdsp is not in this GStreamer");
        gst_object_unref(factory);
        static std::atomic<bool> release{false};
        release.store(false);
        // A build that does not finish until the test says so.
        calls::noise::setSuppressorFactoryForTest(
            [](calls::noise::Mode) -> std::unique_ptr<calls::noise::NoiseSuppressor> {
                QElapsedTimer waited;
                waited.start();
                while (!release.load() && waited.elapsed() < 10000)
                    QThread::msleep(5);
                return nullptr;
            });
        const QString cid = QStringLiteral("cid-audio");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setNoiseSuppressionMode(calls::noise::Mode::WebRtc);
        engine.start();
        engine.publishAudio(cid);
        const auto dspSuppresses = [&] {
            return engine.publishedBinPropertyForTest(
                cid, QStringLiteral("micdsp"),
                QStringLiteral("noise-suppression"));
        };
        QCOMPARE(dspSuppresses(), QStringLiteral("true"));

        engine.setNoiseSuppressionMode(calls::noise::Mode::DeepFilterNet);
        QTest::qWait(400);
        QCOMPARE(dspSuppresses(), QStringLiteral("true"));
        QCOMPARE(engine.lastDspFollowUpForTest(), QString());
        // A report for a mode already left changes nothing.
        engine.handleDenoiseStatus(QStringLiteral("rnnoise"),
                                   QStringLiteral("rnnoise"), true, 960);
        QCOMPARE(engine.lastDspFollowUpForTest(), QString());
        QCOMPARE(dspSuppresses(), QStringLiteral("true"));

        // The backend failed: WebRTC suppression carries on in its place
        // rather than raw noise going out, and the failure says so.
        // (webrtcdsp's PROPERTY cannot show a decision here: in test-source
        // mode webrtcbin parks the stream, so the idle probe that would
        // replace webrtcdsp never fires. The decision is read instead.)
        QSignalSpy status(&engine, &SfuMediaEngine::noiseSuppressionStatus);
        release.store(true);
        engine.handleDenoiseStatus(QStringLiteral("deepfilternet"),
                                   QStringLiteral("off"), false, 0);
        QCOMPARE(engine.lastDspFollowUpForTest(), QStringLiteral("webrtc"));
        QCOMPARE(dspSuppresses(), QStringLiteral("true"));
        QVERIFY(engine.noiseSuppressionFellBackToWebrtc());
        QCOMPARE(status.count(), 1);
        QCOMPARE(status.last().at(0).toString(), QStringLiteral("deepfilternet"));
        QVERIFY(!status.last().at(1).toBool());

        // A backend that runs takes over from webrtcdsp.
        engine.handleDenoiseStatus(QStringLiteral("deepfilternet"),
                                   QStringLiteral("deepfilternet"), true, 1920);
        QCOMPARE(engine.lastDspFollowUpForTest(),
                 QStringLiteral("deepfilternet"));
        QVERIFY(!engine.noiseSuppressionFellBackToWebrtc());
        engine.stop();
        calls::noise::setSuppressorFactoryForTest({});
    }

    void thePublishedMicrophoneRunsTheSelectedSuppressor_data()
    {
        QTest::addColumn<QString>("key");
        QTest::newRow("off") << QStringLiteral("off");
        QTest::newRow("webrtc") << QStringLiteral("webrtc");
        QTest::newRow("rnnoise") << QStringLiteral("rnnoise");
        QTest::newRow("deepfilternet") << QStringLiteral("deepfilternet");
    }
    void thePublishedMicrophoneRunsTheSelectedSuppressor()
    {
        QFETCH(QString, key);
        const calls::noise::Mode mode = calls::noise::modeFromKey(
            key.toStdString(), calls::noise::Mode::Off);
        GstElementFactory *factory = gst_element_factory_find("webrtcdsp");
        const bool dsp = factory != nullptr;
        if (factory)
            gst_object_unref(factory);
        const QString cid = QStringLiteral("cid-audio");

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setNoiseSuppressionMode(mode);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishAudio(cid);
        QCOMPARE(failed.count(), 0);
        const QString built = engine.lastAudioDescriptionForTest();

        // webrtcdsp suppresses in WebRTC mode only; its gain control stays.
        QCOMPARE(built.contains(QStringLiteral("noise-suppression=true")),
                 dsp && mode == calls::noise::Mode::WebRtc);
        QCOMPARE(built.contains(QStringLiteral("noise-suppression=false")),
                 dsp && mode != calls::noise::Mode::WebRtc);
        QVERIFY2(built.contains(
                     QStringLiteral("lightningdenoise name=micdenoise mode=%1")
                         .arg(key)),
                 qPrintable(built));
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdenoise"), QStringLiteral("mode")),
                 key);
        if (dsp) {
            QCOMPARE(engine.publishedBinPropertyForTest(
                         cid, QStringLiteral("micdsp"),
                         QStringLiteral("noise-suppression")),
                     mode == calls::noise::Mode::WebRtc
                         ? QStringLiteral("true")
                         : QStringLiteral("false"));
            QCOMPARE(engine.publishedBinPropertyForTest(
                         cid, QStringLiteral("micdsp"),
                         QStringLiteral("gain-control")),
                     QStringLiteral("true"));
            // Suppression before gain control, as WebRTC's own module orders
            // them: the AGC adapts to speech, not to the noise.
            QVERIFY(built.indexOf(QStringLiteral("micdenoise"))
                    < built.indexOf(QStringLiteral("micdsp")));
        }
        // After the mute valve (a muted microphone costs no inference) and
        // before the encoder.
        QVERIFY(built.indexOf(QStringLiteral("micvalve"))
                < built.indexOf(QStringLiteral("micdenoise")));
        QVERIFY(built.indexOf(QStringLiteral("micdenoise"))
                < built.indexOf(QStringLiteral("audioenc")));
        engine.stop();
    }

    // A change of mode reaches the microphone already publishing, and the
    // next chain is built with it.
    void aModeChangeReachesTheRunningMicrophone()
    {
        GstElementFactory *factory = gst_element_factory_find("webrtcdsp");
        const bool dsp = factory != nullptr;
        if (factory)
            gst_object_unref(factory);
        const QString cid = QStringLiteral("cid-audio");

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setNoiseSuppressionMode(calls::noise::Mode::WebRtc);
        engine.start();
        engine.publishAudio(cid);
        QSignalSpy status(&engine, &SfuMediaEngine::noiseSuppressionStatus);

        engine.setNoiseSuppressionMode(calls::noise::Mode::Off);
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdenoise"), QStringLiteral("mode")),
                 QStringLiteral("off"));
        if (dsp) {
            QTRY_COMPARE_WITH_TIMEOUT(
                engine.publishedBinPropertyForTest(
                    cid, QStringLiteral("micdsp"),
                    QStringLiteral("noise-suppression")),
                QStringLiteral("false"), 5000);
        }
        QVERIFY(!status.isEmpty());
        QCOMPARE(status.last().at(0).toString(), QStringLiteral("off"));
        QVERIFY(status.last().at(1).toBool());

        engine.setNoiseSuppressionMode(calls::noise::Mode::RNNoise);
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdenoise"), QStringLiteral("mode")),
                 QStringLiteral("rnnoise"));
        if (dsp) {
            // Still no WebRTC suppression: exactly one suppressor.
            QCOMPARE(engine.publishedBinPropertyForTest(
                         cid, QStringLiteral("micdsp"),
                         QStringLiteral("noise-suppression")),
                     QStringLiteral("false"));
        }
        engine.stop();

        // The setting outlives the call, like the gain.
        QVERIFY(engine.noiseSuppressionMode() == calls::noise::Mode::RNNoise);
        engine.start();
        engine.publishAudio(cid);
        QVERIFY(engine.lastAudioDescriptionForTest().contains(
            QStringLiteral("mode=rnnoise")));
        engine.stop();
    }

    // The multi-input chain parses and runs in GStreamer: a string comparison
    // cannot check property spelling, `(float)` serialization or `<<…>>` array
    // syntax, and a parse failure means no microphone at all.
    void theMultiInputChainParsesAndRuns()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setDeviceChannelsForTest(4);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));

        const QString built = engine.lastAudioDescriptionForTest();
        QVERIFY2(built.contains(QStringLiteral("mix-matrix")),
                 qPrintable(built.left(200)));
        QVERIFY2(built.contains(
                     QStringLiteral("channels=4,channel-mask=(bitmask)0x0")),
                 qPrintable(built.left(200)));
        // The parse is the assertion.
        QVERIFY2(failed.count() == 0,
                 failed.isEmpty()
                     ? "no failure"
                     : qPrintable(failed.first().at(0).toString()));
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-audio")));
        QVERIFY(engine.publishedBinHasElementForTest(
            QStringLiteral("cid-audio"), QStringLiteral("audioenc")));
        engine.stop();
    }

    // On Linux `pulsesrc` must come before `pipewiresrc`: a bundled
    // gst-plugin-pipewire 1.4 against a 1.6 daemon mishandles `target-object`
    // and never leaves `connecting` once a device is chosen.
    void theMicrophonePrefersPulseOverPipeWireOnLinux()
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QSKIP("Linux element order");
#else
        const QStringList order = SfuMediaEngine::microphoneElementsForTest();
        QVERIFY2(!order.isEmpty(), "no microphone elements offered at all");
        QCOMPARE(order.first(), QStringLiteral("pulsesrc"));
        // pipewiresrc stays as the fallback for hosts without pipewire-pulse.
        QVERIFY2(order.contains(QStringLiteral("pipewiresrc")),
                 qPrintable(order.join(QLatin1Char(','))));
#endif
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
        // The engine is reused across calls; a second start must tear the
        // first down rather than stack a second pipeline.
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
        // The path pressing "call" reaches.
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();

        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        // A failure is a category string, never a crash or silence.
        for (const QList<QVariant> &args : failed) {
            qWarning() << "engine reported failure:"
                       << args.at(0).toString();
        }
        QCOMPARE(failed.count(), 0);
        engine.stop();
    }

    void anOfferIsOnlyMadeOnceThereIsMediaAndThenCarriesIt()
    {
        // webrtcbin raises on-negotiation-needed on reaching PLAYING, before
        // any track exists; an offer built then has no media section and
        // LiveKit answers STATE_MISMATCH. Assert no offer before a track is
        // linked, and a media-bearing offer after.
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy offers(&engine, &SfuMediaEngine::localDescription);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);

        engine.start();
        // Let the PLAYING transition raise negotiation-needed.
        QTest::qWait(400);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(offers.count(), 0);   // deferred, nothing to offer yet

        engine.publishAudio(QStringLiteral("cid-audio"));
        QTRY_VERIFY_WITH_TIMEOUT(offers.count() > 0, 5000);
        QCOMPARE(failed.count(), 0);

        // The publisher offer must describe the audio track (an SDP with no
        // media section is ~98 bytes).
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
        // The controller's order: require encryption, clear keys, then
        // publish. With no key the probes must drop frames, never leak them.
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
        // Still no key, so not claiming encryption.
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

    // Element sends its key as soon as it sees our membership, before the SFU
    // join completes, and not again until it rotates. start() runs at that
    // join, so it must keep keys the call already has, including on a second
    // start() of an active engine. Measured live: the joiner heard nobody
    // until Element rotated.
    void aKeyReceivedWhileJoiningSurvivesTheSessionStart()
    {
        const QString ring = QStringLiteral("@c:example.org/DEV");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.clearKeys();   // join(): the call begins
        engine.setInboundKey(ring, 2, QByteArray(16, 'k'));
        engine.start();       // onSfuJoined()
        QVERIFY2(engine.recvCryptorFor(ring)->hasKey(2),
                 "start() dropped a key received while joining");
        engine.start();       // a second session in the same call
        QVERIFY(engine.recvCryptorFor(ring)->hasKey(2));
        // Keys still end with the call.
        engine.stop();
        QVERIFY(!engine.recvCryptorFor(ring)->hasKey(2));
    }

    // Every key arrival and refusal, and every refused sid-to-device binding,
    // is logged: silent failures there look like "I cannot hear them".
    void theKeyLaneSaysWhetherAKeyArrivedOrWasRefused()
    {
        LogCapture log;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();

        engine.setInboundKey(QStringLiteral("@a:example.org:DEV"), 0,
                             QByteArray(32, 'k'));
        QVERIFY2(log.contains("media key ARRIVED"),
                 qPrintable(log.text()));
        QVERIFY2(log.contains("@a:example.org:DEV"), qPrintable(log.text()));

        // Logged once; the key lane is re-reconciled on every refresh tick.
        const int first = log.count("media key ARRIVED");
        engine.setInboundKey(QStringLiteral("@a:example.org:DEV"), 0,
                             QByteArray(32, 'k'));
        QCOMPARE(log.count("media key ARRIVED"), first);

        // A key the cryptor will not take, and a key addressed to nothing.
        log.clear();
        engine.setInboundKey(QStringLiteral("@b:example.org:DEV"), 0,
                             QByteArray(7, 'k'));
        QVERIFY2(log.contains("was REFUSED by the cryptor"),
                 qPrintable(log.text()));
        log.clear();
        engine.setInboundKey(QString(), 0, QByteArray(32, 'k'));
        QVERIFY2(log.contains("DISCARDED"), qPrintable(log.text()));
        engine.stop();
    }

    void anUnbindableKeyRingSaysSoRatherThanFailingQuietly()
    {
        LogCapture log;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        // Without this binding the key sits in one ring and frames consult
        // another.
        engine.noteParticipantIdentity(QString(),
                                       QStringLiteral("@a:example.org:DEV"));
        QVERIFY2(log.contains("could NOT be bound to a sending stream"),
                 qPrintable(log.text()));
        log.clear();
        // The working case stays quiet.
        engine.noteParticipantIdentity(QStringLiteral("PA_sid"),
                                       QStringLiteral("@a:example.org:DEV"));
        QVERIFY2(!log.contains("could NOT be bound"), qPrintable(log.text()));
        engine.stop();
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
        // 256, not 99: the ring has 256 indices (element-call rotates
        // modulo 256).
        engine.setOutboundKey(256, QByteArray(32, 'k'));
        QVERIFY(!engine.encryptionActive());
        engine.stop();
    }

    void screenShareWithoutASourceIsRefusedNotGuessed()
    {
        // A negative PipeWire node id means "any source", which publishes the
        // wrong monitor.
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

    // Neither media backend may call gst_init itself: GST_PLUGIN_PATH is read
    // once, during the first init, and it must be the bundled path. Checked in
    // source because which init runs first cannot be observed in-process.
    void neitherMediaBackendInitialisesGstreamerItself()
    {
        const QString root = QStringLiteral(SOURCE_DIR "/src/calls/");
        for (const QString &name : { QStringLiteral("SfuMediaEngine.cpp"),
                                     QStringLiteral("GstCallMediaBackend.cpp") }) {
            QFile file(root + name);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(root + name));
            QString source = QString::fromUtf8(file.readAll());
            QVERIFY(!source.isEmpty());
            // Strip whole-line comments first: both files explain this defect
            // in prose above the fix, so a raw-text ban would match them.
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
        // The bootstrap sets the plugin path before it initialises.
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

    // Each platform names a capture element that exists there. Compile-time
    // branches, so only this build's branch is observable; packaging
    // validation checks the others against the shipped plugins.
    void everyPlatformNamesACaptureSourceItActuallyHas()
    {
        const QString camera = SfuMediaEngine::cameraSource();
        const QString screen = SfuMediaEngine::screenShareSource(0, -1);
        QVERIFY(!camera.isEmpty());
        QVERIFY(!screen.isEmpty());
#if defined(Q_OS_WIN)
        // ksvideosrc/gdiscreencapsrc: the mediafoundation and d3d11 plugins do
        // not load in the packaging toolchain. Their property names differ
        // (`monitor`/`cursor` vs `monitor-index`/`show-cursor`).
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
        // Never a Linux element off Linux, nor a Windows one on Linux.
#if !defined(Q_OS_LINUX)
        QVERIFY2(!camera.contains(QStringLiteral("v4l2"))
                     && !screen.contains(QStringLiteral("pipewire")),
                 "a Linux-only capture element is named on a platform that "
                 "does not have it, so the pipeline can never be built");
#endif
    }

    // The camera prefers its MJPG mode (raw YUY2 at 720p saturates USB 2.0 at
    // 10 fps). The JPEG chain must build where its elements exist, and the raw
    // chain must still build where they do not.
    void theCameraJpegChainBuildsWhereItsElementsExist()
    {
        const bool available = SfuMediaEngine::jpegCameraChainAvailable();

        // The probe must match reality: a build without libgstjpeg reports
        // false.
        GstElementFactory *jpegdec = gst_element_factory_find("jpegdec");
        QCOMPARE(available, jpegdec != nullptr);
        if (jpegdec)
            gst_object_unref(jpegdec);

        if (available) {
            // It parses as a real bin, as the engine uses it.
            const QString entry = SfuMediaEngine::cameraJpegEntry();
            QVERIFY2(entry.contains(QStringLiteral("image/jpeg")),
                     "the chain must ASK for jpeg, or the camera's MJPG mode "
                     "can never be negotiated");
            // `jpegenc` stands in for an MJPG camera; raw videotestsrc output
            // cannot link to an `image/jpeg` filter.
            const QString description =
                QStringLiteral("videotestsrc name=capsrc ! jpegenc ! ")
                + entry + QStringLiteral(" ! fakesink");
            GError *error = nullptr;
            GstElement *bin = gst_parse_bin_from_description(
                description.toUtf8().constData(), TRUE, &error);
            if (error) {
                const QString message = QString::fromUtf8(
                    error->message ? error->message : "?");
                g_clear_error(&error);
                QFAIL(qPrintable(QStringLiteral(
                    "the JPEG camera chain does not build: %1").arg(message)));
            }
            QVERIFY(bin);
            gst_object_unref(bin);
        }

        // The raw fallback always builds: it serves cameras without MJPG and
        // is what the engine retries when the JPEG description fails.
        const QString rawDescription =
            QStringLiteral("videotestsrc name=capsrc ! ")
            + SfuMediaEngine::captureEntryFilter(false)
            + QStringLiteral(" ! fakesink");
        GError *rawError = nullptr;
        GstElement *rawBin = gst_parse_bin_from_description(
            rawDescription.toUtf8().constData(), TRUE, &rawError);
        if (rawError) {
            const QString message = QString::fromUtf8(
                rawError->message ? rawError->message : "?");
            g_clear_error(&rawError);
            QFAIL(qPrintable(QStringLiteral(
                "the raw camera chain does not build: %1").arg(message)));
        }
        QVERIFY(rawBin);
        gst_object_unref(rawBin);
    }

    // In a sandbox the camera comes from the xdg Camera portal's PipeWire
    // remote: a Flatpak has no `/dev/video*`. Pins the element's shape; not
    // live-validated.
    void aSandboxedCameraCapturesThroughThePortalsPipeWireRemote()
    {
        const QString direct = SfuMediaEngine::cameraSource();
        const QString portal = SfuMediaEngine::cameraSource(11);
        QVERIFY(!direct.isEmpty());
        QVERIFY(!portal.isEmpty());

        // Without a portal fd, the source is unchanged.
        QCOMPARE(SfuMediaEngine::cameraSource(-1), direct);

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        // No portal on these platforms: an fd must change nothing.
        QCOMPARE(portal, direct);
#else
        QCOMPARE(direct, QStringLiteral("v4l2src"));
        QVERIFY2(portal.startsWith(QStringLiteral("pipewiresrc")),
                 qPrintable(QStringLiteral("not a PipeWire capture: %1")
                                .arg(portal)));
        QVERIFY2(portal.contains(QStringLiteral("fd=11")),
                 qPrintable(QStringLiteral("the portal's remote descriptor is "
                                           "not passed: %1").arg(portal)));

        // No `path=`: the Camera portal grants a remote whose nodes are chosen
        // by autoconnect, so a host node id would resolve to nothing and the
        // pipeline would play without ever producing a buffer.
        QVERIFY2(!portal.contains(QStringLiteral("path=")),
                 qPrintable(QStringLiteral(
                     "a node id from another remote cannot name a node in the "
                     "portal's: %1").arg(portal)));
        QVERIFY2(!portal.contains(QStringLiteral("autoconnect=false")),
                 "autoconnect is what selects the camera on this route; "
                 "turning it off leaves the stream connected to nothing");

        // min-buffers is pinned: gst-plugin-pipewire's default changed from 8
        // to 1 in 1.6, and PipeWire >= 1.6 rejects a buffer range it cannot
        // intersect with the source's. 1 intersects every ceiling.
        QVERIFY2(portal.contains(QStringLiteral("min-buffers=1")),
                 "the portal camera must pin min-buffers, not inherit the "
                 "bundled plugin's version-dependent default");
        for (const QString &banned : { QStringLiteral("min-buffers=5"),
                                       QStringLiteral("min-buffers=8") }) {
            QVERIFY2(!portal.contains(banned),
                     qPrintable(QStringLiteral(
                         "%1 exceeds the buffer ceiling a source can offer; "
                         "measured to stop a PipeWire capture entirely")
                                    .arg(banned)));
        }
        // keepalive-time froze the capture on its first frame.
        QVERIFY2(!portal.contains(QStringLiteral("keepalive-time")),
                 "keepalive-time is back; it was measured to freeze a "
                 "PipeWire capture on its first frame");

        // The frames must not alias PipeWire's buffers: a RECONFIGURE
        // (webrtcbin sends one when the answer lands) makes pipewiresrc free
        // them while they are still queued. Measured in the Flatpak: the
        // camera crashed 2 of 4 starts, and 8 of 8 under repeated
        // reconfigures without the copy, 0 of 5 with it.
        QVERIFY2(portal.contains(QStringLiteral("always-copy=true")),
                 qPrintable(QStringLiteral(
                     "the portal camera hands PipeWire's own buffers "
                     "downstream; a renegotiation frees them under "
                     "videoconvert: %1").arg(portal)));
        // Not the direct route: v4l2src owns its pool.
        QVERIFY(!direct.contains(QStringLiteral("always-copy")));
#endif
    }

    // The portal camera never sees a fixed frame rate anywhere in its
    // description: a pinned rate propagates to pipewiresrc, and PipeWire
    // refuses a mode the camera lacks (`error set output format: -22`).
    // It also gets the raw entry, not the MJPG chain: the fallback only
    // catches parse failures, and an unsupported `image/jpeg` fails later at
    // negotiation.
    void thePortalCameraNeverPinsAFrameRateOnPipeWire()
    {
        const QString portalDescription =
            SfuMediaEngine::videoPipelineDescription(
                SfuMediaEngine::cameraSource(11),
                SfuMediaEngine::cameraRateStage(true),
                SfuMediaEngine::cameraLimitsCaps(true),
                QStringLiteral("vp8enc"), QString(), 1234,
                QStringLiteral("videoconvert ! videoscale"),
                SfuMediaEngine::portalCameraEntry());
        QVERIFY2(!portalDescription.contains(QStringLiteral("framerate")),
                 qPrintable(QStringLiteral(
                     "a fixed frame rate reaches pipewiresrc and PipeWire "
                     "refuses the camera (-22): %1").arg(portalDescription)));

        // One caps structure: a list dies on a camera that cannot meet its
        // first alternative.
        const QString entry = SfuMediaEngine::portalCameraEntry();
        QVERIFY2(!entry.contains(QLatin1Char(';')),
                 qPrintable(QStringLiteral("a caps list in front of "
                                           "pipewiresrc: %1").arg(entry)));
        QVERIFY(entry.contains(QStringLiteral("video/x-raw")));
        // A range with a floor: PipeWire fixates a range to its smallest mode.
        QVERIFY(entry.contains(QStringLiteral("width=(int)[640,")));

        // The rate is bounded, not pinned, and keeps the element name other
        // code looks up.
        const QString rate = SfuMediaEngine::cameraRateStage(true);
        QVERIFY(rate.contains(QStringLiteral("max-rate=30")));
        QVERIFY(rate.contains(QStringLiteral("name=vidrate")));
        QVERIFY(rate.contains(QStringLiteral("skip-to-first=true")));

        // The direct (v4l2src) route is unchanged.
        QCOMPARE(SfuMediaEngine::cameraRateStage(false),
                 SfuMediaEngine::videoRateStage(false));
        QVERIFY(SfuMediaEngine::cameraLimitsCaps(false)
                    .contains(QStringLiteral("framerate=(fraction)30/1")));
    }
    void thePortalCameraIsNotOfferedTheMjpgChain()
    {
        const QByteArray source = SOURCE_UNDER_TEST;
        QVERIFY(!source.isEmpty());
        const int at = source.indexOf("const bool tryJpeg");
        QVERIFY2(at > 0, "the MJPG gate has been renamed; this case is "
                         "asserting against nothing");
        const QByteArray gate = source.mid(at, 120);
        QVERIFY2(gate.contains("pipewireFd < 0"),
                 qPrintable(QStringLiteral(
                     "the MJPG chain is still tried for a portal camera, and "
                     "its fallback cannot catch a negotiation failure: %1")
                                .arg(QString::fromUtf8(gate))));
    }

    // The stored device choice is not applied to a portal camera: it is a
    // host device id, and the portal's PipeWire remote has its own ids.
    void aPortalCameraIsNotBoundToAHostDeviceId()
    {
        const QByteArray source = SOURCE_UNDER_TEST;
        QVERIFY(!source.isEmpty());
        const int at = source.indexOf("const DeviceChoice camera = cameraChoice()");
        QVERIFY2(at > 0, "the camera device-binding site has moved; this case "
                         "is asserting against nothing");
        // Every condition on the stored choice (binding it, or refusing a
        // missing one) must exempt the portal, whose camera it never names.
        const int end = source.indexOf("\n    }\n", at);
        QVERIFY(end > at);
        int checked = 0;
        for (const QByteArray &line : source.mid(at, end - at).split('\n')) {
            if (!line.contains("if (") || !line.contains("camera."))
                continue;
            ++checked;
            QVERIFY2(line.contains("pipewireFd < 0"),
                     qPrintable(QStringLiteral(
                         "the stored camera choice is applied to the portal's "
                         "pipewiresrc: %1").arg(QString::fromUtf8(line))));
        }
        QVERIFY2(checked >= 2, "expected the binding and the missing-camera "
                               "refusal conditions");
    }

    void aScreenShareCaptureUsesThePortalsOwnPipeWireRemote()
    {
        const QString withFd = SfuMediaEngine::screenShareSource(42, 7);
        QVERIFY2(withFd.contains(QStringLiteral("fd=7")),
                 qPrintable(QStringLiteral("no remote fd in: %1").arg(withFd)));
        QVERIFY(withFd.contains(QStringLiteral("path=42")));
        // The fd comes before the path: pipewiresrc resolves the path against
        // whichever remote it has been given.
        QVERIFY(withFd.indexOf(QStringLiteral("fd="))
                < withFd.indexOf(QStringLiteral("path=")));

        // Pinned on both branches: gst-plugin-pipewire's default changed from
        // 8 to 1 in 1.6, KWin offers at most 4 buffers, and PipeWire 1.6
        // rejects a minimum above the source's maximum ("error alloc buffers:
        // Invalid argument").
        QVERIFY2(withFd.contains(QStringLiteral("min-buffers=1")),
                 "the fd branch must pin min-buffers, not inherit the "
                 "bundled plugin's version-dependent default");

        const QString withoutFd = SfuMediaEngine::screenShareSource(42, -1);
        QVERIFY2(withoutFd.contains(QStringLiteral("min-buffers=1")),
                 "the no-fd branch must pin min-buffers too");
        QVERIFY(!withoutFd.contains(QStringLiteral("fd=")));
        QVERIFY(withoutFd.contains(QStringLiteral("path=42")));
        // Values above what a compositor offers (KWin: RANGE(3, 2, 4)) stop
        // the capture entirely.
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
        // keepalive-time froze the share on its first frame; the opening hold
        // is handled downstream in videoRateStage instead.
        QVERIFY2(!withFd.contains(QStringLiteral("keepalive-time")),
                 "keepalive-time is back; it was measured to freeze the "
                 "capture on its first frame");
    }

    // The engine owns the descriptor, so a refusal must close it or every
    // declined share leaks an fd.
    void arefusedScreenShareClosesTheDescriptorItWasGiven()
    {
        int fds[2] = { -1, -1 };
        QCOMPARE(::pipe(fds), 0);
        // fds[0] is handed over; fds[1] stays ours to clean up.
        SfuMediaEngine engine;
        engine.setTestSourceMode(false);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        // A negative node id is refused, and the descriptor is still released.
        engine.publishVideo(QStringLiteral("cid-screen"),
                            /*screenShare=*/true, /*nodeId=*/-1, fds[0]);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(::fcntl(fds[0], F_GETFD), -1);
        QCOMPARE(errno, EBADF);
        engine.stop();
        ::close(fds[1]);
    }

    // A real encrypted call between two engines in LiveKit's topology: A's
    // publisher wired to B's subscriber, with real webrtcbin, ICE, DTLS-SRTP,
    // Opus and AES-GCM frame encryption. If this passes, a failure against a
    // real SFU is an interop fault.
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

        // The same key on both sides, with encryption required: a wrong frame
        // format would show as zero decrypted frames.
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
        // Bind the key's name to the sid the SDP carried, as SfuCallController
        // does from the participant list.
        receiver.noteParticipantIdentity(arrivedStream,
                                         QStringLiteral("sender-device"));

        // Zero encrypted means the capture or encoder never ran; encrypted but
        // zero decrypted means the connection or format is wrong.
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

        // The capture's level reaches the engine. Asserted here because only
        // this harness moves media: a publisher with no peer stalls after its
        // first buffer and `level` never posts.
        QTRY_VERIFY2_WITH_TIMEOUT(
            sender.micPeakDbForTest() < 0.0,
            qPrintable(QStringLiteral("no level report reached the engine; "
                                      "encrypted=%1")
                           .arg(sender.framesEncrypted())),
            30000);
        // Test-source mode publishes a 0.05 sine, about -26 dBFS.
        QVERIFY2(sender.micPeakDbForTest()
                     > SfuMediaEngine::kMicSilenceCeilingDb,
                 qPrintable(QStringLiteral("a 0.05 sine measured %1 dBFS")
                                .arg(sender.micPeakDbForTest())));
        QVERIFY(!sender.microphoneSilentForTest());

        // The receive bin is registered, so it can be retired later.
        QCOMPARE(receiver.receiveBinsForTest(), 1);

        sender.stop();
        receiver.stop();

        // Stopping leaves no outstanding teardown.
        QCOMPARE(receiver.pendingTeardownsForTest(), 0);
        QCOMPARE(sender.pendingTeardownsForTest(), 0);
        // And every webrtcbin reaches NULL, none while it was gathering.
        QTRY_COMPARE_WITH_TIMEOUT(sender.retiringWebrtcForTest(), 0,
                                  kRetiredWithinMs);
        QTRY_COMPARE_WITH_TIMEOUT(receiver.retiringWebrtcForTest(), 0,
                                  kRetiredWithinMs);
        QCOMPARE(sender.teardownsWhileGatheringForTest(), 0);
        QCOMPARE(receiver.teardownsWhileGatheringForTest(), 0);
    }

    // A microphone whose timestamps run ahead of the clock (wasapi2src on a
    // Remote Desktop microphone ran 1.9 % fast, 2026-09-30) must still reach
    // the far end in real time, with honest RTP timestamps. webrtcbin syncs
    // each input to the clock, so without the hold the leaky queue in front
    // of the encoder threw nearly everything away: 100 buffers a second in,
    // 6 out. Restamping the capture itself (the first fix) kept the audio
    // flowing but made every RTP step short (~480 per 20 ms packet here).
    // The source runs at twice real time so the lead builds in a fraction of
    // a second.
    void aMicrophoneAheadOfTheClockStillSendsInRealTime()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        // Not live, paced by the identity: 10 ms of audio every 5 ms.
        sender.setMicrophoneSourceForTest(QStringLiteral(
            "audiotestsrc is-live=false wave=sine freq=440 volume=0.05 "
            "samplesperbuffer=480 ! audio/x-raw,rate=48000,channels=1 "
            "! identity sleep-time=5000"));

        QString failure;
        const auto note = [&failure](const QString &why) {
            if (failure.isEmpty())
                failure = why;
        };
        connect(&sender, &SfuMediaEngine::failed, this, note);
        connect(&receiver, &SfuMediaEngine::failed, this, note);
        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Publisher)
                        && kind == QStringLiteral("offer")) {
                        receiver.applyRemoteDescription(
                            SfuMediaEngine::Target::Subscriber, kind, sdp);
                    }
                });
        connect(&receiver, &SfuMediaEngine::localDescription, &sender,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Subscriber)
                        && kind == QStringLiteral("answer")) {
                        sender.applyRemoteDescription(
                            SfuMediaEngine::Target::Publisher, kind, sdp);
                    }
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
        QString arrivedStream;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &, const QString &) {
                    arrivedStream = streamId;
                });

        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("sender-device"), 3, key);
        sender.publishAudio(QStringLiteral("cid-ahead-audio"));

        // RTP timestamp steps as webrtcbin receives them. Opus at 48 kHz in
        // 20 ms packets steps 960; a lost packet only makes a step longer.
        struct RtpSteps {
            std::atomic<quint64> steps{0};
            std::atomic<quint64> shortSteps{0};
            bool have = false;
            guint32 last = 0;
        };
        auto rtpSteps = std::make_shared<RtpSteps>();
        GstElement *bin =
            sender.publishedBinForTest(QStringLiteral("cid-ahead-audio"));
        QVERIFY(bin);
        GstElement *rtpCaps = gst_bin_get_by_name(GST_BIN(bin), "micrtpcaps");
        QVERIFY(rtpCaps);
        GstPad *rtpPad = gst_element_get_static_pad(rtpCaps, "src");
        gst_object_unref(rtpCaps);
        QVERIFY(rtpPad);
        gst_pad_add_probe(
            rtpPad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *info, gpointer user) {
                auto *s = static_cast<std::shared_ptr<RtpSteps> *>(user)->get();
                GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
                if (buffer && gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
                    const guint32 ts = gst_rtp_buffer_get_timestamp(&rtp);
                    gst_rtp_buffer_unmap(&rtp);
                    if (s->have) {
                        ++s->steps;
                        if (gint32(ts - s->last) < 960)
                            ++s->shortSteps;
                    }
                    s->have = true;
                    s->last = ts;
                }
                return GST_PAD_PROBE_OK;
            },
            new std::shared_ptr<RtpSteps>(rtpSteps),
            [](gpointer user) {
                delete static_cast<std::shared_ptr<RtpSteps> *>(user);
            });
        gst_object_unref(rtpPad);

        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrivedStream.isEmpty(),
            qPrintable(QStringLiteral("no media pad; failure=%1").arg(failure)),
            45000);
        receiver.noteParticipantIdentity(arrivedStream,
                                         QStringLiteral("sender-device"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 0,
            qPrintable(QStringLiteral("nothing decrypted; sent=%1 failure=%2")
                           .arg(sender.framesEncrypted())
                           .arg(failure)),
            45000);
        // Past the start, so the lead has had time to build.
        QTest::qWait(1500);
        const quint64 receivedBefore = receiver.framesDecrypted();
        const quint64 stepsBefore = rtpSteps->steps.load();
        const quint64 shortBefore = rtpSteps->shortSteps.load();
        QTest::qWait(3000);
        const quint64 received = receiver.framesDecrypted() - receivedBefore;
        const quint64 steps = rtpSteps->steps.load() - stepsBefore;
        const quint64 shortSteps = rtpSteps->shortSteps.load() - shortBefore;
        qInfo("ahead source over 3 s: %llu frames received, %llu RTP steps, "
              "%llu short, %llu packets held",
              static_cast<unsigned long long>(received),
              static_cast<unsigned long long>(steps),
              static_cast<unsigned long long>(shortSteps),
              static_cast<unsigned long long>(sender.micBuffersHeldToClock()));
        // At the far end: real time is 150 frames in 3 s and this source
        // offers 300; without the hold a handful arrive. 100 leaves room for
        // a loaded machine.
        QVERIFY2(received >= 100,
                 qPrintable(QStringLiteral("%1 frames reached the receiver in "
                                           "3 s from a source ahead of the "
                                           "clock (sent %2, held %3)")
                                .arg(received)
                                .arg(sender.framesEncrypted())
                                .arg(sender.micBuffersHeldToClock())));
        // Honest RTP clock: never a step shorter than one packet's samples.
        QVERIFY2(steps >= 50, qPrintable(QStringLiteral("%1 RTP steps seen")
                                             .arg(steps)));
        QVERIFY2(shortSteps == 0,
                 qPrintable(QStringLiteral("%1 of %2 RTP timestamp steps were "
                                           "shorter than 960")
                                .arg(shortSteps)
                                .arg(steps)));
        QVERIFY(sender.micBuffersHeldToClock() > 0);

        sender.stop();
        receiver.stop();
    }

    // The hold is a no-op for a source that keeps time: a live source never
    // stamps a buffer later than the clock, whatever latency it reports.
    void aMicrophoneThatKeepsTimeIsNeverHeld()
    {
        using lightning::calls::captureIsAhead;
        using lightning::calls::captureRunningTimeHeld;
        const GstClockTime none = GST_CLOCK_TIME_NONE;
        const GstClockTime ms = GST_MSECOND;
        // Ahead means later than the clock by more than the tolerance.
        QVERIFY(!captureIsAhead(-40 * qint64(ms)));
        QVERIFY(!captureIsAhead(0));
        QVERIFY(!captureIsAhead(20 * qint64(ms)));
        QVERIFY(captureIsAhead(21 * qint64(ms)));
        // Not ahead: untouched, even when clocksync would wait for it.
        QCOMPARE(captureRunningTimeHeld(5000 * ms, 5000 * ms, 40 * ms, false,
                                        none),
                 5000 * ms);
        // Ahead: released at once, i.e. the clock minus the latency
        // clocksync adds on top.
        QCOMPARE(captureRunningTimeHeld(5300 * ms, 5000 * ms, 40 * ms, true,
                                        none),
                 4960 * ms);
        QCOMPARE(captureRunningTimeHeld(5300 * ms, 5000 * ms, 0, true, none),
                 5000 * ms);
        // A packet already older than that is never pushed later.
        QCOMPARE(captureRunningTimeHeld(4900 * ms, 5000 * ms, 40 * ms, true,
                                        none),
                 4900 * ms);
        // Monotonic: the capture turns ahead just after a packet was handed
        // on at 4990 ms; the next is not stepped back to 4960 ms.
        QCOMPARE(captureRunningTimeHeld(5050 * ms, 5000 * ms, 40 * ms, true,
                                        4990 * ms),
                 4990 * ms);
        // Not ahead: untouched, even below the last one (a restarted source).
        QCOMPARE(captureRunningTimeHeld(100 * ms, 5010 * ms, 40 * ms, false,
                                        4970 * ms),
                 100 * ms);

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);   // audiotestsrc is-live=true
        SfuMediaEngine receiver;
        receiver.setTestSourceMode(true);
        connect(&engine, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Publisher)
                        && kind == QStringLiteral("offer")) {
                        receiver.applyRemoteDescription(
                            SfuMediaEngine::Target::Subscriber, kind, sdp);
                    }
                });
        connect(&receiver, &SfuMediaEngine::localDescription, &engine,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Subscriber)
                        && kind == QStringLiteral("answer")) {
                        engine.applyRemoteDescription(
                            SfuMediaEngine::Target::Publisher, kind, sdp);
                    }
                });
        connect(&engine, &SfuMediaEngine::localCandidate, &receiver,
                [&](int target, const QString &init) {
                    if (target == int(SfuMediaEngine::Target::Publisher)) {
                        receiver.applyRemoteCandidate(
                            SfuMediaEngine::Target::Subscriber, init);
                    }
                });
        connect(&receiver, &SfuMediaEngine::localCandidate, &engine,
                [&](int target, const QString &init) {
                    if (target == int(SfuMediaEngine::Target::Subscriber)) {
                        engine.applyRemoteCandidate(
                            SfuMediaEngine::Target::Publisher, init);
                    }
                });
        engine.start();
        receiver.start();
        engine.publishAudio(QStringLiteral("cid-in-time-audio"));
        QTRY_VERIFY_WITH_TIMEOUT(engine.framesEncrypted() >= 100, 45000);
        QCOMPARE(engine.micBuffersHeldToClock(), quint64(0));
        engine.stop();
        receiver.stop();
    }

    // An unkeyed sender is reported as unkeyed, not as a decryption failure:
    // the probe must check the sender's own key ring, not an engine-wide flag
    // set by any sender's key. Driven end to end through a real call.
    void anUnkeyedSenderIsReportedAsUnkeyedRatherThanUndecryptable()
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
        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Publisher)
                        && kind == QStringLiteral("offer")) {
                        receiver.applyRemoteDescription(
                            SfuMediaEngine::Target::Subscriber, kind, sdp);
                    }
                });
        connect(&receiver, &SfuMediaEngine::localDescription, &sender,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Subscriber)
                        && kind == QStringLiteral("answer")) {
                        sender.applyRemoteDescription(
                            SfuMediaEngine::Target::Publisher, kind, sdp);
                    }
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
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &, const QString &, const QString &) {
                    trackArrived = true;
                });
        // Watch the signal the call controller listens to, driven by real
        // frames through the real probe.
        QSignalSpy blocked(&receiver, &SfuMediaEngine::remoteMediaBlocked);
        QVERIFY(blocked.isValid());

        LogCapture log;
        sender.start();
        receiver.start();

        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        // A key for somebody else: the engine holds a key, but the ring the
        // arriving frames consult is empty.
        receiver.setInboundKey(QStringLiteral("a-different-participant"), 3,
                               key);

        sender.publishAudio(QStringLiteral("cid-unkeyed"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            trackArrived,
            qPrintable(QStringLiteral("no media pad; failure=%1").arg(failure)),
            45000);
        // Frames must reach the probe, or the assertion below is vacuous.
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDropped() > 0,
            qPrintable(QStringLiteral("nothing was dropped, so the probe "
                                      "never ran; sent=%1 failure=%2")
                           .arg(sender.framesEncrypted())
                           .arg(failure)),
            30000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            log.contains("DROPPED because no media key"),
            qPrintable(QStringLiteral(
                "the receive path did not report a MISSING KEY. log:\n%1")
                           .arg(log.text())),
            15000);
        QVERIFY2(!log.contains("will not DECRYPT"),
                 qPrintable(QStringLiteral(
                     "an unkeyed sender was reported as a decryption "
                     "failure, which is the defect. log:\n%1")
                                .arg(log.text())));
        QCOMPARE(receiver.framesDecrypted(), quint64(0));

        // The UI is told, with a key-distribution reason. Queued to the
        // engine's thread, so wait for it.
        QTRY_VERIFY2_WITH_TIMEOUT(
            blocked.count() > 0,
            qPrintable(QStringLiteral(
                "%1 frames were dropped for want of a key and the call "
                "header was never told. log:\n%2")
                           .arg(receiver.framesDropped())
                           .arg(log.text())),
            20000);
        const QList<QVariant> raised = blocked.first();
        QVERIFY2(!raised.at(0).toString().isEmpty(),
                 "the badge was raised without naming a stream, so no tile "
                 "can carry it");
        QCOMPARE(raised.at(1).toString(), QStringLiteral("no_key"));

        sender.stop();
        receiver.stop();
    }

    // A peer encrypting into a call we believe is clear (`!required &&
    // !haveKey`) passes ciphertext through; the engine must be able to report
    // it. It fails safe (garbage, never plaintext), so it is instrumented
    // rather than dropped on a two-byte heuristic.
    void aPeerEncryptingIntoACallWeBelieveIsClearIsReported()
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
        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Publisher)
                        && kind == QStringLiteral("offer")) {
                        receiver.applyRemoteDescription(
                            SfuMediaEngine::Target::Subscriber, kind, sdp);
                    }
                });
        connect(&receiver, &SfuMediaEngine::localDescription, &sender,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Subscriber)
                        && kind == QStringLiteral("answer")) {
                        sender.applyRemoteDescription(
                            SfuMediaEngine::Target::Publisher, kind, sdp);
                    }
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
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &, const QString &, const QString &) {
                    trackArrived = true;
                });
        QSignalSpy blocked(&receiver, &SfuMediaEngine::remoteMediaBlocked);
        QVERIFY(blocked.isValid());

        LogCapture log;
        sender.start();
        receiver.start();

        // The sender encrypts; the receiver requires nothing (a room without
        // `m.room.encryption`) and has no inbound key.
        sender.setEncryptionRequired(true);
        sender.setOutboundKey(3, QByteArray(32, 'k'));
        QCOMPARE(receiver.framesArrivingEncryptedOnAClearCall(), quint64(0));

        sender.publishAudio(QStringLiteral("cid-clear-but-encrypted"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            trackArrived,
            qPrintable(QStringLiteral("no media pad; failure=%1").arg(failure)),
            45000);
        // Frames must take the clear path, or everything below is vacuous.
        // `framesDecrypted` is the counter that branch bumps.
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 100,
            qPrintable(QStringLiteral("only %1 frames reached the clear "
                                      "path; sent=%2 failure=%3")
                           .arg(receiver.framesDecrypted())
                           .arg(sender.framesEncrypted())
                           .arg(failure)),
            45000);
        QCOMPARE(receiver.framesDropped(), quint64(0));

        // A rate, not a count: `looksEncrypted` is a two-byte heuristic that
        // real cleartext sometimes passes.
        const quint64 passed = receiver.framesDecrypted();
        const quint64 shaped = receiver.framesArrivingEncryptedOnAClearCall();
        QVERIFY2(shaped * 10 >= passed * 9,
                 qPrintable(QStringLiteral(
                     "%1 of %2 frames passed through in the clear carried "
                     "the frame-crypto trailer — the peer is encrypting and "
                     "this call cannot say so. log:\n%3")
                                .arg(shaped).arg(passed).arg(log.text())));

        QTRY_VERIFY2_WITH_TIMEOUT(
            log.contains("the sender IS encrypting"),
            qPrintable(QStringLiteral(
                "%1 ciphertext frames were handed to the decoder on a call "
                "we believe is clear and nothing said so. log:\n%2")
                           .arg(shaped).arg(log.text())),
            15000);

        // Not the blocked badge: these frames were passed on, not blocked.
        // What to show the user is still open; this pins only the report.
        QCOMPARE(blocked.count(), 0);

        sender.stop();
        receiver.stop();
    }

    // A track published mid-call reaches the receiver: a second subscriber
    // offer adds a section that must be answered while media is flowing.
    void aSecondTrackPublishedMidCallReachesTheReceiver()
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

        int subscriberOffers = 0;
        int subscriberAnswers = 0;
        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target != int(SfuMediaEngine::Target::Publisher)
                        || kind != QStringLiteral("offer")) {
                        return;
                    }
                    ++subscriberOffers;
                    receiver.applyRemoteDescription(
                        SfuMediaEngine::Target::Subscriber, kind, sdp);
                });
        connect(&receiver, &SfuMediaEngine::localDescription, &sender,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target != int(SfuMediaEngine::Target::Subscriber)
                        || kind != QStringLiteral("answer")) {
                        return;
                    }
                    ++subscriberAnswers;
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

        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });

        sender.start();
        receiver.start();

        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        // Keyed by the stream id the publisher's msid carries (the cid).
        receiver.setInboundKey(QStringLiteral("first"), 3, key);
        receiver.setInboundKey(QStringLiteral("second"), 3, key);

        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.size() >= 1,
            qPrintable(QStringLiteral("the first track never arrived; "
                                      "failure=%1").arg(failure)),
            45000);
        const int decryptedBefore = receiver.framesDecrypted();
        QTRY_VERIFY_WITH_TIMEOUT(
            receiver.framesDecrypted() > decryptedBefore, 15000);

        // Deafen first, so the arriving track must honour a mute set before
        // its bin existed. Only the apply in onPadAdded can do that, and it
        // must look up the element by its full `outvol_<stream>_<trackKey>`
        // name.
        receiver.setOutputMuted(true);

        // Now publish a section that was not in the first offer, while the
        // connection is carrying media.
        sender.publishAudio(QStringLiteral("second"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.size() >= 2,
            qPrintable(QStringLiteral("a track published mid-call never "
                                      "reached the receiver: offers=%1 "
                                      "answers=%2 arrived=%3 failure=%4")
                           .arg(subscriberOffers).arg(subscriberAnswers)
                           .arg(arrived.join(QLatin1Char(',')), failure)),
            45000);
        QVERIFY2(arrived.contains(QStringLiteral("second")),
                 qPrintable(QStringLiteral("the mid-call track was "
                                           "misattributed: %1")
                                .arg(arrived.join(QLatin1Char(',')))));
        QCOMPARE(receiver.receiveMutedForTest(QStringLiteral("second")), 1);

        sender.stop();
        receiver.stop();
    }

    // A participant volume set before the receive bin exists is remembered
    // and applied when the bin is built; the miss is logged once.
    void aVolumeChosenBeforeTheTrackArrivesLandsWhenItDoes()
    {
        LogCapture log;
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

        QString offeredStream;
        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target != int(SfuMediaEngine::Target::Publisher)
                        || kind != QStringLiteral("offer")) {
                        return;
                    }
                    receiver.applyRemoteDescription(
                        SfuMediaEngine::Target::Subscriber, kind, sdp);
                    // The publisher's msid is its cid and the receiver keys the
                    // stream by it, so the stream id is known before any pad,
                    // bin or frame exists here.
                    if (offeredStream.isEmpty()) {
                        offeredStream = QStringLiteral("cid-loopback-audio");
                        receiver.setParticipantVolume(offeredStream, 0);
                        // Twice: the second miss must not log again.
                        receiver.setParticipantVolume(offeredStream, 0);
                    }
                });
        connect(&receiver, &SfuMediaEngine::localDescription, &sender,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Subscriber)
                        && kind == QStringLiteral("answer")) {
                        sender.applyRemoteDescription(
                            SfuMediaEngine::Target::Publisher, kind, sdp);
                    }
                });
        connect(&sender, &SfuMediaEngine::localCandidate, &receiver,
                [&](int target, const QString &init) {
                    if (target == int(SfuMediaEngine::Target::Publisher))
                        receiver.applyRemoteCandidate(
                            SfuMediaEngine::Target::Subscriber, init);
                });
        connect(&receiver, &SfuMediaEngine::localCandidate, &sender,
                [&](int target, const QString &init) {
                    if (target == int(SfuMediaEngine::Target::Subscriber))
                        sender.applyRemoteCandidate(
                            SfuMediaEngine::Target::Publisher, init);
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
        QVERIFY2(!offeredStream.isEmpty(), "the offer carried no msid");
        QCOMPARE(arrivedStream, offeredStream);
        // The bin exists now, and carries the earlier volume.
        QTRY_COMPARE_WITH_TIMEOUT(receiver.receiveVolumeForTest(arrivedStream),
                                  0.0, 5000);
        // A later change goes straight to the element.
        log.clear();
        receiver.setParticipantVolume(arrivedStream, 100);
        QVERIFY(receiver.receiveVolumeForTest(arrivedStream) > 0.0);
        // The landing is logged, so a live run can assert on it.
        QVERIFY2(log.contains("participant volume applied"),
                 qPrintable(QStringLiteral("no landing line; log was:\n%1")
                                .arg(log.text())));
        QVERIFY2(log.contains("elements= 1"),
                 qPrintable(QStringLiteral("the landing named no element "
                                           "count; log was:\n%1")
                                .arg(log.text())));
        QCOMPARE(log.count("participant volume applied"), 1);
        // The same value again is not logged: a slider drag re-sends it.
        receiver.setParticipantVolume(arrivedStream, 100);
        QCOMPARE(log.count("participant volume applied"), 1);
        // A real change is.
        receiver.setParticipantVolume(arrivedStream, 60);
        QCOMPARE(log.count("participant volume applied"), 2);
        sender.stop();
        receiver.stop();
        QCOMPARE(receiver.receiveVolumeForTest(arrivedStream), -1.0);
    }

    // Encrypted VP8 survives payload -> depayload through our payloader.
    // GStreamer's rtpvp8pay parses the bitstream and fails on ciphertext.
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
                // The payloaded bytes must decrypt back to the plain frame.
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

    // A keyframe request (PLI -> upstream GstForceKeyUnit) must reach vp8enc
    // through our payloader, or a late subscriber waits for the encoder's own
    // keyframe schedule.
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

        // Drain the opening keyframe and a few deltas, so the next keyframe
        // can only come from the request.
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
                    // Only a frame's first packet carries the VP8 frame tag.
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

    // Reassembled as libwebrtc does (start on S, end on marker), every frame
    // must equal the encoder's output; GStreamer's depayloader is more lenient.
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

    // GST_BUFFER_FLAG_DELTA_UNIT (which picks our clear-header length) must
    // match the VP8 frame tag's P bit that other clients decrypt by.
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

    // Our packets round-trip through GStreamer's depayloader and carry a
    // 15-bit picture id, which LiveKit's SFU needs to rewrite the descriptor.
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
        // One start and one marker per frame, and a distinct picture id per
        // frame: a munging SFU cannot forward a constant one.
        QCOMPARE(starts, markers);
        // Chrome starts the picture id at a random value, never 0: the SFU
        // seeds its wrap handler with `PictureID - 1`.
        QVERIFY2(!pictureIds.contains(0),
                 "the picture id sequence started at 0");
        QVERIFY2(pictureIds.size() == starts,
                 qPrintable(QStringLiteral("%1 frames carried %2 distinct "
                                           "picture ids")
                                .arg(starts).arg(pictureIds.size())));
        // The RTP timestamp advances per frame: libwebrtc's jitter buffer
        // groups frames by timestamp, while GStreamer's depayloader only
        // looks at the marker bit.
        QVERIFY2(frameTimestamps.size() >= 3,
                 "too few frames to judge the timestamps");
        for (int i = 1; i < frameTimestamps.size(); ++i) {
            QVERIFY2(frameTimestamps.at(i) != frameTimestamps.at(i - 1),
                     qPrintable(QStringLiteral(
                         "frames %1 and %2 share RTP timestamp %3")
                             .arg(i - 1).arg(i).arg(frameTimestamps.at(i))));
        }
    }

    // The encoder gets a fixed cadence, never a range containing 0/1: a
    // desktop capture negotiates `framerate=0/1` (PipeWire delivers on
    // damage), and vp8enc needs a rate to plan against.
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

    // A bus error from a live or unknown bin is not a publish failure:
    // pipelines post errors during ordinary teardown. Only the narrow case of
    // a capture that never delivered is escalated.
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
        // videotestsrc delivers buffers, so this capture is alive and an error
        // from it is a transient elsewhere.
        QTest::qWait(400);
        engine.handlePublishError(QStringLiteral("cid-video"));
        QCOMPARE(publish.count(), 0);

        // The "unpublished cid is not reportable" clause is covered against an
        // audio bin in publishingTwiceUnderOneIdIsIgnored(); unpublishing this
        // bin deadlocks (see unpublishingALiveVideoBinDoesNotDeadlock()).
        engine.stop();
    }

    /// Volume curve: 0-100 is literal, 100-200 expands to 100-1000%.
    /// Attenuation must stay 1:1; a linear 0-200 slider would top out at +6 dB.
    void theVolumeCurveIsLiteralBelowUnityAndExpandsAbove()
    {
        // Attenuation is untouched; 0 must be exactly 0.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(0), 0);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(1), 1);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(50), 50);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(99), 99);
        // Unity on both sides of the join, so there is no step at 100.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(100), 100);
        // Boost: the far end of the slider is the element's own ceiling.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(200), 1000);
        // Monotonic in between, with the midpoint on the straight line.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(150), 550);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(101), 109);
        // Out of range saturates rather than wrapping or extrapolating past
        // the element's range, where it would clamp silently.
        QCOMPARE(SfuMediaEngine::audioFactorPercent(-5), 0);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(10000), 1000);
    }

    /// The renegotiated offer stops advertising an unpublished track's msid;
    /// that is what makes the far end drop the tile. A mute removes nothing.
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

        // The section survives but goes inactive. Releasing the request pad
        // alone drops the msid but leaves `a=sendrecv`, and an m= section may
        // never be removed from an SDP.
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

    /// Stopping a share retires its transceiver: the m= section count comes
    /// back down, and a re-publish reuses the slot instead of stacking.
    /// Otherwise the far end keeps rendering the last frame.
    void stoppingAPublishRetiresItsTransceiver()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        // -1, not 0: the publisher webrtcbin is built lazily on first publish.
        QCOMPARE(engine.publisherTrackSlotsForTest(), -1);

        engine.publishVideo(QStringLiteral("cid-share"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        QTest::qWait(400);
        QCOMPARE(engine.publisherTrackSlotsForTest(), 1);

        // Teardown is asynchronous (see the deadlock case below), so the count
        // falls shortly after return.
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

        // The next share must not stack on a new section.
        engine.publishVideo(QStringLiteral("cid-share-2"),
                            /*screenShare=*/false, /*nodeId=*/-1);
        QTest::qWait(400);
        QCOMPARE(engine.publisherTrackSlotsForTest(), 1);
        engine.stop();
    }

    /// Unpublishing a live video bin must not deadlock: the state change on
    /// the main thread waited for a stream lock held by a pad probe on the
    /// streaming thread. The pad is blocked with an IDLE probe and the state
    /// change runs via gst_element_call_async.
    ///
    /// The 400 ms wait is the test: unpublishing before the bin streams does
    /// not reproduce it. Reordering unlink/unparent before set_state(NULL) does
    /// not fix it, since neither stops a push already in flight.
    void unpublishingALiveVideoBinDoesNotDeadlock()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        QTest::qWait(400);
        engine.unpublish(QStringLiteral("cid-video"));
        // Reaching here is the assertion. The republish proves the cid can be
        // reused.
        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        QTest::qWait(200);
        engine.stop();
    }

    /// Nothing pops either pipeline bus (sync handler, no watch, no main
    /// loop), so the handler must not leave messages queued: each
    /// STATE_CHANGED holds a ref on its element and a long call piles them up.
    void thePipelineBusesAreNotLeftToAccumulate()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"), /*screenShare=*/false,
                            /*nodeId=*/-1);
        // Long enough for both pipelines to reach PLAYING.
        QTest::qWait(600);
        QVERIFY2(engine.busesWithPendingMessagesForTest() == 0,
                 "a pipeline bus is holding messages nobody will ever read: "
                 "the sync handler is passing them into the async queue and "
                 "each one pins a reference on the element that posted it");
        engine.stop();
    }

    /// An engine destroyed mid-teardown leaves nothing running. The deferred
    /// teardown unparents the bin before stopping it, so destroyPeer() cannot
    /// reach it, and its crypto probes hold raw pointers into the engine. The
    /// counter is shared so it can be read after the engine is gone.
    void anEngineDestroyedMidTeardownLeavesNothingRunning()
    {
        std::shared_ptr<const std::atomic<int>> outstanding;
        int armed = 0;
        {
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.start();
            engine.publishVideo(QStringLiteral("cid-video"),
                                /*screenShare=*/false, /*nodeId=*/-1);
            engine.publishAudio(QStringLiteral("cid-audio"));
            // The wait matters: a bin that has not started streaming does not
            // take the deferred path.
            QTest::qWait(400);
            outstanding = engine.teardownCounterForTest();
            engine.unpublish(QStringLiteral("cid-video"));
            engine.unpublish(QStringLiteral("cid-audio"));
            // No event loop from here to the closing brace: the teardowns are
            // in flight when the destructor runs.
            armed = outstanding->load();
        }
        QVERIFY2(armed > 0,
                 "no teardown was outstanding when the engine was destroyed, "
                 "so this run proved nothing; the deferred path did not arm");
        QVERIFY2(outstanding->load() == 0,
                 "the engine was destroyed with a publish teardown still in "
                 "flight: that bin is unparented, still running, and its "
                 "crypto probes point at members that have just been "
                 "destroyed");
        // And nothing lands afterwards either.
        QTest::qWait(200);
        QCOMPARE(outstanding->load(), 0);
    }

    /// A teardown completion from a closed session must not touch the next
    /// one: completions carry (token, generation) and are filtered by
    /// tokenIsLive(), or they close the new session's descriptor under the
    /// same cid.
    void aTeardownCompletionFromAClosedSessionLeavesTheNextAlone()
    {
        int fds[2] = { -1, -1 };
        QCOMPARE(::pipe(fds), 0);
        const QString cid = QStringLiteral("cid-share");

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishVideo(cid, /*screenShare=*/false, /*nodeId=*/-1);
        QTest::qWait(400);
        engine.unpublish(cid);
        // stop() waits for the teardown, so the completion is now queued on
        // this thread.
        engine.stop();

        // A new session publishing the same cid with its own descriptor.
        // fds[0] is handed over; fds[1] stays ours.
        engine.start();
        engine.publishVideo(cid, /*screenShare=*/false, /*nodeId=*/-1, fds[0]);
        QVERIFY2(::fcntl(fds[0], F_GETFD) != -1,
                 "the new share's descriptor was closed before the stale "
                 "completion could even land, so this case proves nothing");

        // Now let the stale completion land.
        QTest::qWait(400);
        QVERIFY2(::fcntl(fds[0], F_GETFD) != -1,
                 "a teardown completion from the previous call closed the "
                 "new share's PipeWire descriptor: the capture dies and "
                 "nothing says why");
        engine.stop();
        ::close(fds[1]);
    }

    /// A publish that cannot link leaves nothing behind: the cid is freed, the
    /// bin unparented and the webrtcbin request pad released (a leftover sink
    /// pad is a transceiver, i.e. an empty m= section). The failure is
    /// injected because webrtcbin always grants the pad.
    void aPublishThatCannotLinkLeavesNothingRegistered()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        // One real track first, so the slot count measures the failed publish.
        engine.publishAudio(QStringLiteral("cid-audio"));
        QTest::qWait(300);
        QCOMPARE(engine.publisherTrackSlotsForTest(), 1);
        QCOMPARE(failed.count(), 0);

        engine.failNextPublishLinkForTest();
        engine.publishShareAudio(QStringLiteral("cid-share-audio"));
        QVERIFY2(!engine.hasPublishedBinForTest(
                     QStringLiteral("cid-share-audio")),
                 "share audio left its bin registered after the link failed: "
                 "the cid can never be published again and a dead bin stays "
                 "in the live publisher pipeline");
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(),
                 QStringLiteral("share_audio_failed"));
        QVERIFY2(engine.publisherTrackSlotsForTest() == 1,
                 "the webrtcbin request pad was not given back, so the next "
                 "offer carries an m=audio section with nothing behind it");

        // The cid is free again.
        engine.publishShareAudio(QStringLiteral("cid-share-audio"));
        QTest::qWait(300);
        QVERIFY2(engine.hasPublishedBinForTest(
                     QStringLiteral("cid-share-audio")),
                 "share audio could not be republished under the cid its own "
                 "failure had taken");
        QCOMPARE(engine.publisherTrackSlotsForTest(), 2);
        engine.stop();
    }

    // A publish that never prerolls is reported without ending the call.
    // Staged with an unresolvable PipeWire node id and no portal remote.
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
        // A capture device never ends a call; asserted before any skip.
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
        // One report, not a storm.
        QTest::qWait(600);
        QCOMPARE(publish.count(), 1);
        engine.stop();
    }

    // The rate stage must not change the picture's geometry. A stage that only
    // re-times leaves sink width == src width; `compositor` (tried once to fix
    // the opening hold) is not a scaler and crops a 4K capture to its top-left
    // quarter.
    void theRateStageNeverCropsTheCapture()
    {
        for (bool screenShare : {false, true}) {
            const QString stage =
                SfuMediaEngine::videoRateStage(screenShare);
            const QString desc =
                QStringLiteral(
                    "videotestsrc num-buffers=2 "
                    "! video/x-raw,width=3840,height=2160,framerate=30/1 "
                    // No `name=` here: videoRateStage() names its own element
                    // and gst_parse keeps the first `name=`.
                    "! videoconvert ! videoscale ! %1 "
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
                                                   "vidrate");
            QVERIFY2(rate,
                     "the rate stage no longer builds an element called "
                     "`vidrate` — see videoRateStage()");
            // Iterate the pads: compositor's sink pads are request pads
            // (sink_%u), so a static "sink" lookup would return null.
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

            // Not a skip: both sides negotiated at PLAYING, so a zero means
            // the test could not measure.
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

    // Pins a known open defect: videorate emits nothing from a single capture
    // buffer. If this starts failing the hold is fixed; invert it only
    // together with theRateStageNeverCropsTheCapture().
    void theOpeningHoldIsStillPresentAndUnfixed()
    {
        const int frames =
            framesFromASingleCaptureBuffer(QStringLiteral("videorate"), 800);
        QVERIFY2(frames >= 0, "the videorate harness did not run");
        QCOMPARE(frames, 0);
    }

    // One capsfilter, after the rate stage: a size range before it lets a
    // 3440x1440 source negotiate past the 1920 ceiling.
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
            // Exactly one occurrence, or the ceiling is negotiated twice.
            QCOMPARE(desc.count(limits), 1);
        }
    }

    // The blocked badge's decision policy. Asserts the announcement rather
    // than a log line.
    void theBlockedBadgeNeedsARateNotABadFrameOrARun()
    {
        using Policy = SfuMediaEngine::BlockedRunPolicy;

        // One bad frame in three thousand never raises the badge.
        {
            Policy p;
            bool raise = false;
            int announcements = 0;
            for (int i = 0; i < 3102; ++i) {
                const bool failed = (i == 1500);
                if (p.note(failed, &raise))
                    ++announcements;
            }
            QCOMPARE(announcements, 0);
            QVERIFY(!p.announced);
        }

        // A consecutive-run gate is reset by any good frame; a stream with one
        // usable frame in fifty (98% failure) must still raise.
        {
            Policy p;
            bool raise = false;
            bool raised = false;
            for (int i = 0; i < 600; ++i) {
                if (p.note(/*failed=*/(i % 50) != 0, &raise) && raise)
                    raised = true;
            }
            QVERIFY2(raised,
                     "a stream failing 98% of its frames never badged — that "
                     "is the consecutive-run hole, and it is worse than the "
                     "defect it replaced");
        }

        // A total key mismatch raises: nothing before kMinObserved, raised by
        // the time the window is full.
        {
            Policy p;
            bool raise = false;
            int raisedAt = -1;
            for (int i = 0; i < Policy::kWindow; ++i) {
                if (p.note(/*failed=*/true, &raise) && raise && raisedAt < 0)
                    raisedAt = i;
            }
            QCOMPARE(raisedAt, Policy::kMinObserved - 1);
        }

        // It clears once keys arrive.
        {
            Policy p;
            bool raise = false;
            for (int i = 0; i < Policy::kWindow; ++i)
                p.note(/*failed=*/true, &raise);
            QVERIFY(p.announced);
            bool cleared = false;
            for (int i = 0; i < Policy::kWindow; ++i) {
                if (p.note(/*failed=*/false, &raise) && !raise)
                    cleared = true;
            }
            QVERIFY2(cleared, "the badge never came off a recovered stream");
            QVERIFY(!p.announced);
        }

        // Hysteresis: once raised it stays raised until the failure rate
        // falls to the clear threshold.
        {
            Policy p;
            bool raise = false;
            for (int i = 0; i < Policy::kWindow; ++i)
                p.note(/*failed=*/true, &raise);
            QVERIFY(p.announced);
            int moves = 0;
            // 50% failures: below the raise threshold, above the clear one.
            for (int i = 0; i < Policy::kWindow * 4; ++i) {
                if (p.note(/*failed=*/(i % 2) == 0, &raise))
                    ++moves;
            }
            QCOMPARE(moves, 0);
            QVERIFY(p.announced);
        }

        // The thresholds are ordered, or the cases above are vacuous.
        QVERIFY(Policy::kClearPercent < Policy::kRaisePercent);
        QVERIFY(Policy::kMinObserved <= Policy::kWindow);
    }

    // Every queue on a live media path is bounded, and leaky unless it
    // carries RTP into a depayloader: a default `queue` holds 1 s and never
    // leaks it. Source scan with per-file counts across all three files that
    // build pipelines, so a queue moving between files cannot hide.
    void everyLiveQueueIsBoundedAndLeaky()
    {
        struct Lane { const char *path; int expected; };
        const QList<Lane> lanes = {
            { SOURCE_DIR "/src/calls/SfuMediaEngine.cpp", 7 },
            { SOURCE_DIR "/src/calls/ShareAudioSources.cpp", 2 },
            { SOURCE_DIR "/src/calls/GstCallMediaBackend.cpp", 2 },
            // The Settings microphone test's voice playback.
            { SOURCE_DIR "/src/calls/AudioDeviceTester.cpp", 1 },
        };

        // A pipeline queue is followed by a pad separator or by any
        // `property=`; a C++ variable named `queue` is followed by ` = `.
        static const QRegularExpression queueElement(QStringLiteral(
            "\\bqueue(?=\\s+(?:!|[a-z][a-z0-9-]*=))"));

        for (const Lane &lane : lanes) {
            QFile file(QString::fromUtf8(lane.path));
            QVERIFY2(file.open(QIODevice::ReadOnly), lane.path);
            QString code = QString::fromUtf8(file.readAll());
            QVERIFY(!code.isEmpty());
            code.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        // Splice adjacent string literals so a queue's properties on the next
        // source line read as one element. Quote-whitespace-quote cannot match
        // the `", "` between array entries.
            code.remove(QRegularExpression(QStringLiteral("\"\\s*\"")));

            int found = 0;
            QRegularExpressionMatchIterator it = queueElement.globalMatch(code);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                ++found;
                const QString tail = code.mid(m.capturedStart(), 260);
                const QString props = tail.section(QLatin1Char('!'), 0, 0);
                const QString next = tail.section(QLatin1Char('!'), 1, 1);
                QVERIFY2(props.contains(QStringLiteral("max-size-time="))
                             || props.contains(
                                 QStringLiteral("max-size-buffers=")),
                         qPrintable(QStringLiteral(
                             "%1: a queue on a live path carries no explicit "
                             "bound: '%2'")
                                        .arg(QString::fromUtf8(lane.path),
                                             props.trimmed())));
                // Exempt: a queue feeding a video depayloader holds RTP, and
                // leaking there corrupts the bitstream downstream of webrtcbin
                // (no PLI). The appsink's `drop=true` bounds its latency.
                if (next.contains(QStringLiteral("rtpvp8depay")))
                    continue;
                QVERIFY2(props.contains(QStringLiteral("leaky=downstream")),
                         qPrintable(QStringLiteral(
                             "%1: a queue on a live path is not leaky: '%2' "
                             "— a default queue holds one second and never "
                             "drains")
                                        .arg(QString::fromUtf8(lane.path),
                                             props.trimmed())));
            }
            QCOMPARE(found, lane.expected);
        }
    }

    // 2026-10-04 "call audio pops" TODO: the receive chain's own drops must be
    // countable. Fails on the old tree (no named queue/decoder, no counters).
    void theAudioReceiveChainIsInstrumentedForLossAndConcealment()
    {
        QFile file(QStringLiteral(SOURCE_DIR "/src/calls/SfuMediaEngine.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString code = QString::fromUtf8(file.readAll());
        // Both audio descriptions name the elements the counters attach to.
        QCOMPARE(code.count(QStringLiteral("queue name=recvqueue")), 2);
        QCOMPARE(code.count(QStringLiteral("opusdec name=recvdec")), 2);
        QVERIFY(code.contains(QStringLiteral("\"overrun\"")));
        QVERIFY(code.contains(QStringLiteral("GstRTPPacketLost")));
        QVERIFY(code.contains(QStringLiteral("audio receive health stream=")));
        QVERIFY(code.contains(QStringLiteral("jitterbuffer ")));
    }

    // Group calls ignored the chosen output device: the engine stored
    // speakerChoice() and nothing read it. The receive sink must take the
    // resolved speaker; fails on the old tree (function absent, sink was always
    // autoaudiosink / recvsink).
    void aChosenOutputReplacesTheDefaultReceiveSink()
    {
        SfuMediaEngine::OutputSink chosen;
        chosen.sink = QStringLiteral("pulsesink name=outsink");
        chosen.binding.property = QStringLiteral("device");
        chosen.binding.value = QStringLiteral("alsa_output.test");
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(QString(), &chosen),
                 QStringLiteral("pulsesink name=outsink"));
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(
                     QStringLiteral("client"), &chosen),
                 QStringLiteral("pulsesink name=outsink"));
        // System default (no binding) and no choice at all keep today's sinks.
        SfuMediaEngine::OutputSink fallback;
        fallback.sink = QStringLiteral("autoaudiosink name=outsink");
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(QString(), &fallback),
                 QStringLiteral("autoaudiosink"));
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(QString(), nullptr),
                 QStringLiteral("autoaudiosink"));
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(
                     QStringLiteral("client"), nullptr),
                 QStringLiteral("pulsesink name=recvsink"));
        // And production actually asks for it.
        QFile file(QStringLiteral(SOURCE_DIR "/src/calls/SfuMediaEngine.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString code = QString::fromUtf8(file.readAll());
        // The receive path reads the cache; the device monitor must never run
        // on the streaming thread that builds a new participant's bin.
        const int begin = code.indexOf(
            QStringLiteral("bool SfuMediaEngine::prepareReceiveChain("));
        const int end = code.indexOf(
            QStringLiteral("bool SfuMediaEngine::startReceiveChain("), begin);
        QVERIFY(begin > 0 && end > begin);
        const QString body = code.mid(begin, end - begin);
        QVERIFY(body.contains(QStringLiteral("cachedSpeakerSink()")));
        QVERIFY2(!body.contains(QStringLiteral("resolveSpeakerSink")),
                 "prepareReceiveChain resolves devices on a streaming thread");
        QVERIFY(code.contains(QStringLiteral("QThreadPool::globalInstance()")));
    }

    // The live shape: about ten undecryptable frames within ~160 ms of a new
    // sender's stream appearing (before its key is installed), then none.
    // That burst must never raise the badge.
    void theLiveBurstAtAStreamsStartNeverRaisesTheBadge()
    {
        using Policy = SfuMediaEngine::BlockedRunPolicy;
        Policy p;
        bool raise = false;
        int announcements = 0;
        // Ten bad frames at the very start, then a long clean call.
        for (int i = 0; i < 5074; ++i) {
            if (p.note(/*failed=*/i < 10, &raise))
                ++announcements;
        }
        QCOMPARE(announcements, 0);
        QVERIFY(!p.announced);

        // Nor may the burst raise it when it is all that has been seen.
        Policy fresh;
        for (int i = 0; i < 10; ++i)
            fresh.note(/*failed=*/true, &raise);
        QVERIFY(!fresh.announced);
        QCOMPARE(fresh.failurePercent(), -1);
    }

    // The camera names its source instead of `autovideosrc`: by rank that
    // picks pipewiresrc with no target on a PipeWire desktop, which fails to
    // negotiate and produces no buffers.
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
        const QString selfView = SfuMediaEngine::selfViewBranch();
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
        // The payloader must be ours: rtpvp8pay parses the bitstream and
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
        // The self-view branch must survive.
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

        // A mid may never be the track key while the SDP holds a track sid
        // for that section. The pad's `msid` property varies between
        // GStreamer releases; the SDP text does not, so read the sid from it.
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
        // ...and the SDP scan must extract it.
        QVERIFY2(pane.contains(
                     QStringLiteral("trackSid = SfuMediaEngine::trackSidFromMsid")),
                 "the SDP scan records no per-section track sid, so the "
                 "fallback above has nothing to read");
        // All three section maps describe one subscriber description and are
        // cleared together: section mids repeat across calls.
        for (const char *map : { "m_streamForMline.clear()",
                                 "m_midForMline.clear()",
                                 "m_trackForMline.clear()" }) {
            QVERIFY2(pane.contains(QLatin1String(map)),
                     qPrintable(QStringLiteral(
                         "%1 is missing: a section map outlives its call")
                                    .arg(QLatin1String(map))));
        }
    }

    // A chosen window on Windows routes to Lightning's own window capture
    // element (gdiscreencapsrc cannot capture a window); ignoring the handle
    // would share the whole desktop. Source-scanned because the branch is
    // Windows-only.
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

        // The node-id guard must not refuse the other source kinds: a window
        // handle or (Linux, no portal) an X11 root rectangle.
        QVERIFY2(pane.contains(QStringLiteral(
                     "if (nodeId < 0 && windowHandle == 0 "
                     "&& !captureRect.isValid()) {")),
                 "the no-source refusal rejects a share that carries one of "
                 "the three source kinds but no node id");

        // The element must be registered, or the description fails to parse.
        QVERIFY2(pane.contains(
                     QStringLiteral("wincap::registerWindowCaptureSrc()")),
                 "the window capture element is never registered");
    }

    // The Linux no-portal fallback captures an X11 root-window rectangle; the
    // arithmetic is the whole correctness of the feature.
    void theLinuxNoPortalFallbackCapturesAnX11RootRectangle()
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QSKIP("the X11 fallback branch does not exist off Linux");
#else
        // A second monitor at an offset.
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
        // ximagesrc's edges are inclusive: `startx=100 endx=1379` gives
        // width 1280.
        QVERIFY2(source.contains(QStringLiteral("endx=1379")),
                 qPrintable(QStringLiteral("endx is not the inclusive right "
                                           "edge: %1").arg(source)));
        QVERIFY2(source.contains(QStringLiteral("endy=769")),
                 qPrintable(QStringLiteral("endy is not the inclusive bottom "
                                           "edge: %1").arg(source)));

        // A display at the origin still names its edges; 0 means "whole
        // root", which spans every monitor.
        const QString atOrigin = SfuMediaEngine::screenShareSource(
            0, -1, 0, QRect(0, 0, 1920, 1080));
        QVERIFY(atOrigin.contains(QStringLiteral("endx=1919")));
        QVERIFY(atOrigin.contains(QStringLiteral("endy=1079")));

        // The portal path is unchanged when no rectangle is given.
        const QString portal = SfuMediaEngine::screenShareSource(42, 7);
        QVERIFY(portal.startsWith(QStringLiteral("pipewiresrc")));
        QVERIFY(!portal.contains(QStringLiteral("ximagesrc")));
        QVERIFY(portal.contains(QStringLiteral("fd=7")));

        // Every property name exists: gst_parse_launch fails on an unknown
        // one. Parsing stays at NULL, so no X server is touched.
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

    // The fallback's capability probe asks the running registry, before the
    // picker is offered.
    void theCaptureElementProbeAsksTheRealRegistry()
    {
        QCOMPARE(QLatin1String(SfuMediaEngine::x11ScreenCaptureElementName()),
                 QLatin1String("ximagesrc"));
        // An element the engine already requires.
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
        // Unpacked (protocol 0): still the participant.
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123 TR_xyz789")),
                 QStringLiteral("PA_abc123"));
        // No track-id token at all.
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123")),
                 QStringLiteral("PA_abc123"));
        // A leading separator names no participant; empty routes and decrypts
        // nothing.
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("|TR_xyz789 TR_xyz789")),
                 QString());
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(QString()), QString());

        // The track sid, which media keys are addressed by, from both shapes.
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123|TR_xyz789 TR_xyz789")),
                 QStringLiteral("TR_xyz789"));
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123 TR_xyz789")),
                 QStringLiteral("TR_xyz789"));
        // Nothing that looks like a track sid: empty, so the caller looks
        // elsewhere.
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123")),
                 QString());
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(QString()), QString());
    }

    // A media key names a Matrix device and a frame names a LiveKit sid; they
    // arrive in either order and must converge on one ring.
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

    // Every LiveKit-shaped subscriber offer is answered: the server will not
    // renegotiate while an answer is outstanding.
    void everyLiveKitSubscriberOfferIsAnswered()
    {
        const QString fp = QStringLiteral(
            "a=fingerprint:sha-256 "
            "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
            "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00");
        const QString dataOnly = QStringLiteral(
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=-\r\n"
            "t=0 0\r\n"
            "a=group:BUNDLE 0\r\n"
            "a=msid-semantic: WMS *\r\n"
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=ice-ufrag:abcd\r\n"
            "a=ice-pwd:0123456789abcdef0123456789\r\n"
            "a=ice-options:trickle\r\n"
            "%1\r\n"
            "a=setup:actpass\r\n"
            "a=mid:0\r\n"
            "a=sctp-port:5000\r\n"
            "a=max-message-size:262144\r\n").arg(fp);
        const QString withAudio = QStringLiteral(
            "v=0\r\n"
            "o=- 1 2 IN IP4 127.0.0.1\r\n"
            "s=-\r\n"
            "t=0 0\r\n"
            "a=group:BUNDLE 0 1\r\n"
            "a=msid-semantic: WMS *\r\n"
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=ice-ufrag:abcd\r\n"
            "a=ice-pwd:0123456789abcdef0123456789\r\n"
            "a=ice-options:trickle\r\n"
            "%1\r\n"
            "a=setup:actpass\r\n"
            "a=mid:0\r\n"
            "a=sctp-port:5000\r\n"
            "a=max-message-size:262144\r\n"
            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=rtcp-mux\r\n"
            "a=ice-ufrag:abcd\r\n"
            "a=ice-pwd:0123456789abcdef0123456789\r\n"
            "a=ice-options:trickle\r\n"
            "%1\r\n"
            "a=setup:actpass\r\n"
            "a=mid:1\r\n"
            "a=sendonly\r\n"
            "a=msid:PA_late TR_late\r\n"
            "a=rtpmap:111 opus/48000/2\r\n"
            "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
            "a=ssrc:11111111 cname:lk\r\n"
            "a=ssrc:11111111 msid:PA_late TR_late\r\n").arg(fp);

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        QSignalSpy local(&engine, &SfuMediaEngine::localDescription);
        engine.start();

        engine.applyRemoteDescription(SfuMediaEngine::Target::Subscriber,
                                      QStringLiteral("offer"), dataOnly);
        QTRY_VERIFY2_WITH_TIMEOUT(
            local.count() >= 1,
            qPrintable(QStringLiteral("the data-channel-only offer was never "
                                      "answered; failures=%1")
                           .arg(failed.count())),
            10000);
        QCOMPARE(local.at(0).at(0).toInt(),
                 int(SfuMediaEngine::Target::Subscriber));
        QCOMPARE(local.at(0).at(1).toString(), QStringLiteral("answer"));

        engine.applyRemoteDescription(SfuMediaEngine::Target::Subscriber,
                                      QStringLiteral("offer"), withAudio);
        QTRY_VERIFY2_WITH_TIMEOUT(
            local.count() >= 2,
            qPrintable(QStringLiteral("no answer to the offer that added a "
                                      "media section; answers=%1 failures=%2")
                           .arg(local.count()).arg(failed.count())),
            10000);
        const QString second = local.at(1).at(2).toString();
        QVERIFY2(second.contains(QLatin1String("m=audio")),
                 "the answer carried no audio section");
        QVERIFY2(!second.contains(QLatin1String("m=audio 0 ")),
                 "the answer REJECTED the audio section (port 0)");
        QCOMPARE(failed.count(), 0);
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

    // videorate starts its output clock at segment start, and the Windows
    // sources stamp with pipeline running time, so a late-published bin would
    // be back-filled with duplicates for the whole call age. Drives the real
    // rate stage (`skip-to-first`).
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

        // A publish starting with the pipeline: 1 s at 10 fps becomes ~1 s at
        // 30.
        const int fresh = measure(0);
        QVERIFY2(fresh > 0, "the rate stage produced nothing at all");
        QVERIFY2(fresh < 60,
                 qPrintable(QStringLiteral("a fresh publish emitted %1 buffers "
                                           "for 1 s of input")
                                .arg(fresh)));

        // The same input into a call up for 174 s: the output count must not
        // change.
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

    // A still window must still publish: PipeWire delivers on damage and
    // videorate needs a second buffer, so the keep-alive injects repeats into
    // the rate stage's real injection pad. The control case (no injection)
    // must publish zero. Arming the timer (tickShareKeepAlive) is not covered.
    void aStillScreenStillPublishesAPicture()
    {
        struct Counter {
            int frames = 0;
        };
        const auto measure = [](bool inject) {
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
            // framerate=0/1 is the screencast's own shape; a fixed input rate
            // would not reproduce the hold.
            GstCaps *inCaps = gst_caps_new_simple(
                "video/x-raw", "format", G_TYPE_STRING, "BGRA", "width",
                G_TYPE_INT, 64, "height", G_TYPE_INT, 64, "framerate",
                GST_TYPE_FRACTION, 0, 1, nullptr);
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
            if (!gst_element_link_many(src, rate, caps, sink, nullptr)) {
                gst_object_unref(pipeline);
                return -1;
            }
            auto *counter = new Counter;
            GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
            gst_pad_add_probe(
                sinkPad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *, GstPadProbeInfo *, gpointer data) {
                    static_cast<Counter *>(data)->frames++;
                    return GST_PAD_PROBE_OK;
                },
                counter, nullptr);
            gst_object_unref(sinkPad);
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            gst_element_get_state(pipeline, nullptr, nullptr,
                                  GST_CLOCK_TIME_NONE);

            const auto push = [src](GstClockTime pts) {
                GstBuffer *buffer =
                    gst_buffer_new_allocate(nullptr, 64 * 64 * 4, nullptr);
                gst_buffer_memset(buffer, 0, 0, 64 * 64 * 4);
                GST_BUFFER_PTS(buffer) = pts;
                GST_BUFFER_DTS(buffer) = pts;
                gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
            };
            // The one frame a still window produces; no EOS, as in a live share.
            push(0);
            QTest::qWait(200);

            if (inject) {
                GstPad *rateSink =
                    SfuMediaEngine::keepAliveInjectionPad(pipeline);
                if (!rateSink) {
                    gst_element_set_state(pipeline, GST_STATE_NULL);
                    gst_object_unref(pipeline);
                    delete counter;
                    return -2;   // the injection point is gone
                }
                // Chain into the rate stage's own sink pad, as
                // tickShareKeepAlive() does. Pushing from an IDLE probe on the
                // upstream peer deadlocks (the probe blocks the push).
                for (int i = 1; rateSink && i <= 10; ++i) {
                    GstBuffer *repeat =
                        gst_buffer_new_allocate(nullptr, 64 * 64 * 4, nullptr);
                    gst_buffer_memset(repeat, 0, 0, 64 * 64 * 4);
                    const GstClockTime at =
                        GstClockTime(i) * (GST_SECOND / 10);
                    GST_BUFFER_PTS(repeat) = at;
                    GST_BUFFER_DTS(repeat) = at;
                    GST_BUFFER_DURATION(repeat) = GST_CLOCK_TIME_NONE;
                    gst_pad_chain(rateSink, repeat);
                    QTest::qWait(30);
                }
                if (rateSink)
                    gst_object_unref(rateSink);
            } else {
                QTest::qWait(300);
            }
            const int frames = counter->frames;
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            delete counter;
            return frames;
        };

        // Control: one buffer and no help publishes nothing.
        const int alone = measure(false);
        QCOMPARE(alone, 0);

        const int helped = measure(true);
        QVERIFY2(helped != -2,
                 "the rate stage is no longer named `vidrate`, so the "
                 "keep-alive has no pad to inject on and a still screen "
                 "publishes nothing");
        QVERIFY2(helped > 0,
                 qPrintable(QStringLiteral("re-pushing the last picture "
                                           "produced %1 buffers — videorate "
                                           "is no longer started by a second "
                                           "buffer, so the keep-alive no "
                                           "longer fixes a still screen")
                                .arg(helped)));
    }

    // The keep-alive PTS stays in the source's timebase: some sources
    // (LightningWindowCaptureSrc, ximagesrc) are zero-based, and a running-time
    // PTS would make videorate drop every real frame after it. Tested on the
    // arithmetic, since a fixture pipeline's clock is also ~0.
    void theKeepAlivePtsNeverLeavesTheSourcesTimebase()
    {
        // A zero-based source three frames in, on a call up for 174 s.
        const quint64 sampled = 3 * (GST_SECOND / 30);
        const quint64 at = SfuMediaEngine::keepAlivePts(
            sampled, /*sampledPtsValid=*/true, /*elapsedMs=*/600,
            /*lastInjectedPts=*/0, /*lastInjectedPtsValid=*/false);
        QVERIFY2(GST_CLOCK_TIME_IS_VALID(at), "no timestamp was produced");
        QCOMPARE(at, sampled + 600 * GST_MSECOND);
        // It stays near the source's clock, so the next real frame is ahead
        // of it.
        QVERIFY2(at < 174 * GST_SECOND,
                 qPrintable(QStringLiteral(
                                "the injected PTS is %1 ns for a source whose "
                                "own clock reads %2 ns — the keep-alive has "
                                "left the source's timebase and will strand "
                                "every zero-based capture")
                                .arg(at)
                                .arg(sampled)));

        // Strictly increasing even when the anchor has not moved.
        const quint64 again = SfuMediaEngine::keepAlivePts(
            sampled, true, 600, at, true);
        QVERIFY2(again > at, "two injections produced a non-advancing PTS");

        // No source PTS means nothing to anchor to: refuse.
        QVERIFY2(!GST_CLOCK_TIME_IS_VALID(SfuMediaEngine::keepAlivePts(
                     0, /*sampledPtsValid=*/false, 600, 0, false)),
                 "the keep-alive invented a timestamp for a source that "
                 "supplied none");
    }

    // The injection point is found by name in the real publish description,
    // as publishVideo() does.
    void theRealPublishDescriptionStillOffersAnInjectionPad()
    {
        const QString description = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("videotestsrc is-live=true"),
            SfuMediaEngine::videoRateStage(/*screenShare=*/true),
            SfuMediaEngine::shareLimitsCaps(1080, 30),
            QStringLiteral("vp8enc deadline=1"), QString(), 1234u,
            SfuMediaEngine::shareScaleStage(1080, /*gpu=*/false),
            SfuMediaEngine::captureEntryFilter(/*gpu=*/false));
        GError *error = nullptr;
        GstElement *bin = gst_parse_bin_from_description(
            description.toUtf8().constData(), TRUE, &error);
        if (error) {
            const QString message = QString::fromUtf8(error->message);
            g_error_free(error);
            if (bin)
                gst_object_unref(bin);
            QFAIL(qPrintable(QStringLiteral("the publish description will "
                                            "not parse: %1")
                                 .arg(message)));
        }
        QVERIFY(bin);
        GstPad *pad = SfuMediaEngine::keepAliveInjectionPad(bin);
        QVERIFY2(pad,
                 "the real publish description no longer contains an element "
                 "called `vidrate`, so the screen-share keep-alive has "
                 "nowhere to inject and a still window publishes nothing");
        // And its peer: publishVideo() installs the sampling probe there, and
        // without it the keep-alive silently never injects (decodebin's
        // delayed linking can leave it missing at parse time).
        GstPad *peer = gst_pad_get_peer(pad);
        QVERIFY2(peer,
                 "`vidrate` has no upstream peer at parse time, so the "
                 "keep-alive's sampling probe is never installed and a still "
                 "window publishes nothing — silently");
        gst_object_unref(peer);
        gst_object_unref(pad);
        gst_object_unref(bin);
    }

    // A shared window's publish size keeps its aspect and never upscales.
    // Compiled on every platform so it is testable off Windows.
    void fitIntoKeepsTheAspectAndNeverUpscales()
    {
        using lightning::wincap::fitInto;

        // A maximised 3840x2100 window against the 1080p ceiling: 1050, not
        // 1080.
        const auto brave = fitInto(3840, 2100, 1920, 1080);
        QCOMPARE(brave.width, 1920);
        QCOMPARE(brave.height, 1050);

        // A window inside the ceiling keeps its size.
        const auto small = fitInto(800, 600, 1920, 1080);
        QCOMPARE(small.width, 800);
        QCOMPARE(small.height, 600);

        // Odd edges round down to even (VP8 subsamples chroma by two).
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

    // The share scale stage, CPU and opt-in GPU, is well formed.
    void theGpuScaleStageIsWellFormedAndOptIn()
    {
        // Both branches: a malformed GPU caps string would otherwise surface
        // only at pipeline build for whoever enabled it.
        for (int h : { 720, 1080, 1440, 2160 }) {
            const QString cpu = SfuMediaEngine::shareScaleStage(h, false);
            // One threaded pass: `videoconvert ! videoscale` walks the pixels
            // twice on one thread and cannot keep up at 4K.
            QCOMPARE(cpu, QStringLiteral("videoconvertscale n-threads=4"));

            const QString gpu = SfuMediaEngine::shareScaleStage(h, true);
            // Imports the compositor's buffer instead of reading back every
            // frame at capture size.
            QVERIFY2(gpu.startsWith(QStringLiteral("glupload")),
                     qPrintable(gpu));
            // glcolorconvert is required: the portal delivers `DMA_DRM`, an
            // opaque buffer the scaler cannot sample, and the share goes out
            // black.
            QVERIFY2(gpu.contains(QStringLiteral("glcolorconvert")),
                     qPrintable(QStringLiteral(
                         "no glcolorconvert: a DMA_DRM buffer reaches the "
                         "scaler unconverted and the share is black: %1")
                             .arg(gpu)));
            // texture-target=2D is required: a DMA-BUF imports as
            // GL_TEXTURE_EXTERNAL_OES, which glcolorscale would sample as
            // empty.
            // No `(ANY)` fallback: it matches DMA-BUF at any modifier, so a
            // block-linear buffer could bypass the linear preference and
            // import as black.
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
            // Pinned before the scaler, or the conversion is already skipped.
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
            // Ranges, so a smaller window is never upscaled.
            QVERIFY2(gpu.contains(QStringLiteral("[1,%1]").arg(h)),
                     qPrintable(gpu));
            QVERIFY2(gpu.contains(QStringLiteral("[1,%1]").arg((h * 16) / 9)),
                     qPrintable(gpu));
            // ASCII digits: the string goes to GStreamer's C parser.
            for (QChar c : gpu) {
                QVERIFY2(!c.isDigit() || (c >= QLatin1Char('0')
                                          && c <= QLatin1Char('9')),
                         "a non-ASCII digit reached the GL caps string");
            }
        }

        // The entry filter. Both forms pin the PAR (an unfixated PAR is taken
        // to 1/2147483647 and overflows videoscale); only the GPU form admits
        // a non-system memory feature, or pipewiresrc is asked for a
        // downloaded buffer before glupload can import it.
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
        // The CPU fallback is one threaded pass (`videoconvertscale
        // n-threads=4`); two serial passes at 4K cannot keep up in realtime.
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
        // The GPU path is Linux-only: `libgstopengl.dll` is not staged on
        // Windows, so the description would fail to parse and there would be
        // no screen share at all.
#if defined(Q_OS_LINUX)
        // The env var is readable here, so the gate is the platform alone.
        QVERIFY2(SfuMediaEngine::shareScaleStage(1080, true)
                     .contains(QStringLiteral("glupload")),
                 "the GPU stage stopped using glupload on Linux");
        // The probe names the missing element rather than answering a
        // boolean, so a packaging gap points at the plugin list.
        {
            const QString missing = SfuMediaEngine::missingGpuShareElement();
            // In the dev shell nothing is missing; the point is that the
            // answer is a name.
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

        // GPU scaling is requested by default where the platform can carry it;
        // availability is probed separately and a parse failure falls back.
#if defined(Q_OS_LINUX)
        QVERIFY2(SfuMediaEngine::shareGpuScalingRequested(),
                 "the GPU share path is no longer attempted by default");
#endif
        // Drive the probe's failing branch: the names are a parameter so the
        // "missing" answer is reachable in a dev shell that has everything.
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

    // The camera keeps its own scale stage (`videoconvert ! videoscale`); the
    // GPU/CPU ladder's fallback must not substitute the share's stage.
    void theCameraKeepsItsOwnScaleStageWhateverTheShareIsDoing()
    {
        // The two stages must be distinguishable, or this case cannot fail.
        const QString shareCpu = SfuMediaEngine::shareScaleStage(2160, false);
        const QString cameraStage = QStringLiteral("videoconvert ! videoscale");
        QVERIFY2(shareCpu != cameraStage,
                 "the share and camera CPU stages became identical, so this "
                 "case can no longer detect the camera being re-pointed");

        // Assert the decision itself, not a description built from a stage
        // passed in by the test.
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(false, 2160),
                 cameraStage);
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(true, 2160), shareCpu);
        // And the share's height must not leak into the camera's answer.
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(false, 720),
                 SfuMediaEngine::cpuFallbackScaleStage(false, 2160));
    }

    void shareCapsAndBitrateAreRightAtEveryOfferedQuality()
    {
        // All nine combinations, against the real derivation rather than
        // hardcoded strings.
        struct Row { int h; int fps; int w; };
        const QList<Row> rows = {
            { 720, 15, 1280 },  { 720, 30, 1280 },  { 720, 60, 1280 },
            { 1080, 15, 1920 }, { 1080, 30, 1920 }, { 1080, 60, 1920 },
            { 1440, 15, 2560 }, { 1440, 30, 2560 }, { 1440, 60, 2560 },
            { 2160, 15, 3840 }, { 2160, 30, 3840 }, { 2160, 60, 3840 },
        };
        for (const Row &r : rows) {
            const QString caps = SfuMediaEngine::shareLimitsCaps(r.h, r.fps);

            // Ranges, or videoscale would upscale a small window.
            QVERIFY2(caps.contains(QStringLiteral("width=(int)[1,%1]")
                                       .arg(r.w)),
                     qPrintable(QStringLiteral("wrong width range at %1p%2: "
                                               "%3").arg(r.h).arg(r.fps)
                                    .arg(caps)));
            QVERIFY2(caps.contains(QStringLiteral("height=(int)[1,%1]")
                                       .arg(r.h)),
                     qPrintable(caps));
            // Fixed framerate: a range including 0/1 leaves vp8enc no rate to
            // plan against.
            QVERIFY2(caps.contains(QStringLiteral("framerate=(fraction)%1/1")
                                       .arg(r.fps)),
                     qPrintable(caps));
            // Fixed PAR: an unfixated PAR is taken to 1/2147483647 and
            // overflows videoscale.
            QVERIFY2(caps.contains(
                         QStringLiteral("pixel-aspect-ratio=(fraction)1/1")),
                     qPrintable(caps));
            // ASCII digits: a localised digit would not parse.
            for (QChar c : caps) {
                QVERIFY2(!c.isDigit() || (c >= QLatin1Char('0')
                                          && c <= QLatin1Char('9')),
                         "a non-ASCII digit reached the caps string");
            }

            const QString enc = SfuMediaEngine::shareEncoderStage(r.h, r.fps);
            // The keyframe interval is in frames and must track the rate.
            QVERIFY2(enc.contains(QStringLiteral("keyframe-max-dist=%1")
                                      .arg(2 * r.fps)),
                     qPrintable(enc));
            // The bitrate stays inside the band: this lane has no congestion
            // control and end-usage=cbr pads to its target.
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

        // 1080p30 is the reference and must not change.
        QVERIFY(SfuMediaEngine::shareEncoderStage(1080, 30)
                    .contains(QStringLiteral("target-bitrate=3000000")));
        QVERIFY(SfuMediaEngine::shareLimitsCaps(1080, 30)
                    .contains(QStringLiteral("width=(int)[1,1920]")));
    }

    void shareAudioIsOfferedOnlyWhenSomethingCanActuallyCaptureIt()
    {
        // Availability comes from the plugins actually present, not the
        // platform macro: a package may ship any subset of capture plugins.
        const bool available = SfuMediaEngine::shareAudioAvailable();

        // Recomputed from the factories rather than copied from the
        // implementation, so the two can disagree.
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
        // Per-application capture does not use a loopback element, so a
        // PipeWire machine without `pulsesrc` is also a yes.
        const bool perApplication =
            lightning::shareaudio::perApplicationCaptureAvailable();
        // A device-monitor filter matches a provider by the classes it
        // advertises, and PipeWire's provider does not advertise
        // "Stream/Output/Audio". Vacuous without PipeWire (CI), real with it.
        const bool haveProvider =
            gst_device_provider_factory_find("pipewiredeviceprovider")
            != nullptr;
        bool haveMixer = false;
        if (GstElementFactory *mix = gst_element_factory_find("audiomixer")) {
            gst_object_unref(mix);
            haveMixer = true;
        }
        // `target-object` is what a branch needs; `on-disconnect` is NOT
        // required (it exists only from PipeWire 1.6, and requiring it
        // switched per-application capture off in every package).
        bool canTarget = false;
        if (GstElementFactory *pw = gst_element_factory_find("pipewiresrc")) {
            if (GstElement *probe = gst_element_factory_create(pw, nullptr)) {
                canTarget = g_object_class_find_property(
                                G_OBJECT_GET_CLASS(probe), "target-object")
                            != nullptr;
                gst_object_unref(probe);
            }
            gst_object_unref(pw);
        }
        // And a daemon the provider actually reached: its start() says yes
        // without one.
        bool reached = false;
        if (haveProvider) {
            if (GstDeviceProvider *provider =
                    gst_device_provider_factory_get_by_name(
                        "pipewiredeviceprovider")) {
                if (gst_device_provider_start(provider)) {
                    GList *devices = gst_device_provider_get_devices(provider);
                    reached = devices != nullptr;
                    g_list_free_full(devices, gst_object_unref);
                    gst_device_provider_stop(provider);
                }
                gst_object_unref(provider);
            }
        }
        if (haveProvider && haveMixer && canTarget && reached) {
            QVERIFY2(perApplication,
                     "this machine has the PipeWire device provider, a "
                     "PipeWire daemon it reaches, audiomixer and pipewiresrc "
                     "target-object, so per-application share audio must be "
                     "available — if it is not, every share is silently "
                     "taking the sink-monitor path and sending the call back "
                     "to the call");
        }
        if (haveProvider && !reached) {
            QVERIFY2(!perApplication,
                     "no PipeWire daemon is reachable, yet per-application "
                     "capture claims to be available: every share would "
                     "carry nothing but the silence floor");
        }
        QVERIFY2(available == (anyPresent || perApplication),
                 qPrintable(QStringLiteral(
                     "shareAudioAvailable()=%1 but loopback=%2 perApp=%3")
                     .arg(available).arg(anyPresent).arg(perApplication)));
        // Whichever capture this machine has, the answer must be yes.
        if (anyPresent || perApplication)
            QVERIFY(available);

        // With no peer, publishing share audio adds no bin: a track announced
        // and never fed shows the far end a participant sharing silence.
        SfuMediaEngine engine;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"));
        QVERIFY2(!engine.hasPublishedBinForTest(
                     QStringLiteral("cid-share-audio")),
                 "share audio registered a bin without a publisher peer");
    }

    // The window capture element implements set_caps and sizes buffers from
    // the negotiated caps; downstream reads at the caps' stride, so a buffer of
    // the window's own size garbles the picture. Source-scanned (Windows-only).
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

        // The buffer size comes from the negotiated fields, never a fresh
        // measurement of the window.
        QVERIFY2(source.contains(QStringLiteral(
                     "static_cast<gsize>(self->outWidth) * self->outHeight")),
                 "the frame size is no longer computed from the negotiated "
                 "output size");

        // The capture measures the window rect, because that is what
        // PrintWindow draws. Match the call form, since a comment may name
        // the bare token.
        QVERIFY2(!source.contains(QStringLiteral("GetClientRect(")),
                 "the capture is measuring the client rect again while "
                 "PrintWindow draws the whole window");

        // The fit uses the shared rule.
        QVERIFY2(source.contains(QStringLiteral("wincap::fitInto(")),
                 "the element no longer uses the shared fitting rule");
    }
    // The camera's MJPG entry: MJPG is the only mode that fits 720p30 through
    // USB 2.0, and it can only be chosen if the first element downstream of
    // the source accepts image/jpeg.
    void theCameraJpegEntryDecodesExplicitlyAndKeepsThePixelAspectPin()
    {
        const QString entry = SfuMediaEngine::cameraJpegEntry();

        // It must accept image/jpeg, or the source cannot offer MJPG.
        QVERIFY2(entry.contains(QStringLiteral("image/jpeg")),
                 "the camera entry does not accept MJPG, so a camera still "
                 "cannot negotiate one");

        // Explicit jpegdec, never decodebin: decodebin's pads appear only once
        // data flows, so delayed linking fails and the capture dies.
        QVERIFY2(entry.contains(QStringLiteral("jpegdec")),
                 "the MJPG entry does not decode explicitly");
        QVERIFY2(!entry.contains(QStringLiteral("decodebin")),
                 "decodebin is refuted for this chain — delayed linking kills "
                 "the capture; use jpegdec, which links at parse time");

        // The PAR pin survives the decode (an unfixated PAR overflows
        // videoscale).
        QVERIFY2(entry.contains(
                     QStringLiteral("pixel-aspect-ratio=(fraction)1/1")),
                 "the MJPG entry dropped the pixel-aspect-ratio pin that the "
                 "raw entry applies");

        // It ends in raw video, which everything downstream expects.
        QVERIFY(entry.contains(QStringLiteral("video/x-raw")));
    }

    // Windows share audio prefers `wasapi2src` with
    // `loopback-mode=exclude-process-tree` and our pid, so participants do not
    // hear themselves back. The mode is ignored unless the pid is non-zero.
    // Source contract: wasapi2src is not available on CI.
    void windowsShareAudioPrefersExcludingOurOwnProcess()
    {
        const QByteArray src = SOURCE_UNDER_TEST;
        QVERIFY2(!src.isEmpty(), "engine source unreadable");

        // Keyed on the table entries themselves, not on prose a comment could
        // also contain.
        const int excluding = src.indexOf("{ \"wasapi2src\", \"loopback-target-pid\"");
        const int plain = src.indexOf("{ \"wasapi2src\", \"loopback\",");
        QVERIFY2(excluding >= 0,
                 "Windows share audio no longer asks WASAPI to exclude this "
                 "process, so a shared screen sends the call back to itself");
        QVERIFY2(plain >= 0,
                 "the older-Windows fallback is gone; a machine below build "
                 "20348 has no share audio at all now");
        QVERIFY2(excluding < plain,
                 "the plain endpoint-mix capture is offered before the one "
                 "that excludes us, so every Windows share takes the echoing "
                 "path");

        // The enum without the pid is a no-op, so the pid must be interpolated.
        const int pidProp = src.indexOf("loopback-target-pid=%1", excluding);
        QVERIFY2(pidProp > excluding && pidProp < plain,
                 "exclude-process-tree is set without a target pid, which the "
                 "element ignores outright");
        QVERIFY2(src.indexOf("loopback-mode=exclude-process-tree", excluding)
                     < plain,
                 "the excluding candidate does not actually ask for the "
                 "exclude mode");
    }

    // A share-audio branch parses on its own, as rescanShareAudioSources()
    // builds it for an application that starts playing mid-share. Bare
    // trailing caps parse only when something follows them.
    void aShareAudioBranchParsesStandaloneTheWayTheDynamicPathBuildsIt()
    {
        // `pipewiresrc` is a separate package; skip rather than fail with an
        // unrelated parse error.
        for (const char *needed : { "pipewiresrc", "queue", "audioconvert",
                                    "audioresample", "capsfilter" }) {
            GstElementFactory *factory = gst_element_factory_find(needed);
            if (!factory)
                QSKIP(qPrintable(QStringLiteral("no %1 in this build")
                                     .arg(QLatin1String(needed))));
            gst_object_unref(factory);
        }

        lightning::shareaudio::Stream s;
        s.serial = QStringLiteral("9551");
        const QString branch =
            lightning::shareaudio::applicationBranchDescription(s, 0);
        QVERIFY(!branch.isEmpty());

        // As rescanShareAudioSources() does when an application starts playing
        // during a share.
        GError *error = nullptr;
        GstElement *bin = gst_parse_bin_from_description(
            branch.toUtf8().constData(), TRUE, &error);
        const QString message =
            error && error->message ? QString::fromUtf8(error->message)
                                    : QString();
        if (error)
            g_error_free(error);
        if (bin)
            gst_object_unref(bin);
        QVERIFY2(message.isEmpty(),
                 qPrintable(QStringLiteral(
                     "GStreamer refused a branch that the dynamic path builds "
                     "verbatim: %1 — every application that begins playing "
                     "during a share is silently left out of it")
                     .arg(message)));

        // The whole description still parses.
        const QString whole = lightning::shareaudio::mixedSourceDescription(
            { s }) + QStringLiteral(" ! fakesink");
        GError *wholeError = nullptr;
        GstElement *wholeBin = gst_parse_bin_from_description(
            whole.toUtf8().constData(), TRUE, &wholeError);
        const QString wholeMessage =
            wholeError && wholeError->message
                ? QString::fromUtf8(wholeError->message)
                : QString();
        if (wholeError)
            g_error_free(wholeError);
        if (wholeBin)
            gst_object_unref(wholeBin);
        QVERIFY2(wholeMessage.isEmpty(), qPrintable(wholeMessage));
    }

    // The share-audio track as publishShareAudio() composes it parses. A
    // description ending in a pad reference takes no `name=` assignment, so
    // appending one breaks the whole bin.
    void theWholeShareAudioTrackParsesTheWayPublishShareAudioComposesIt()
    {
        // Name each needed element: the suite gate only requires webrtcbin,
        // and a missing plugin should skip, not fail with an unrelated parse
        // error. `pipewiresrc` is the most likely to be absent.
        for (const char *needed : { "audiomixer", "audiotestsrc", "audioconvert",
                                    "audioresample", "capsfilter", "valve",
                                    "opusenc", "rtpopuspay", "pipewiresrc" }) {
            GstElementFactory *factory = gst_element_factory_find(needed);
            if (!factory)
                QSKIP(qPrintable(QStringLiteral("no %1 in this build")
                                     .arg(QLatin1String(needed))));
            gst_object_unref(factory);
        }

        lightning::shareaudio::Stream s;
        s.serial = QStringLiteral("9551");

        const auto parseFailure = [](const QString &description) {
            GError *error = nullptr;
            GstElement *bin = gst_parse_bin_from_description(
                description.toUtf8().constData(), TRUE, &error);
            const QString message =
                error && error->message ? QString::fromUtf8(error->message)
                                        : QString();
            if (error)
                g_error_free(error);
            if (bin)
                gst_object_unref(bin);
            return message;
        };

        // Per-application path, with two streams and with none (a share that
        // starts before anything plays carries only the silence floor).
        lightning::shareaudio::Stream other;
        other.serial = QStringLiteral("307");
        for (const QList<lightning::shareaudio::Stream> &streams :
             { QList<lightning::shareaudio::Stream>{},
               QList<lightning::shareaudio::Stream>{ s },
               QList<lightning::shareaudio::Stream>{ s, other } }) {
            const QString whole =
                lightning::shareaudio::encodedTrackDescription(
                    lightning::shareaudio::mixedSourceDescription(streams),
                    0x1234u);
            const QString message = parseFailure(whole);
            QVERIFY2(message.isEmpty(),
                     qPrintable(QStringLiteral(
                         "GStreamer refused the share-audio track with %1 "
                         "application stream(s): %2 — the share publishes no "
                         "audio at all and the engine reports "
                         "share_audio_failed")
                         .arg(streams.size()).arg(message)));
        }

        // The single-element path (pulsesrc, wasapi2src, test source) still
        // composes.
        const QString single = lightning::shareaudio::encodedTrackDescription(
            QStringLiteral("audiotestsrc name=sharesrc is-live=true "
                           "wave=silence"),
            0x1234u);
        QVERIFY2(parseFailure(single).isEmpty(),
                 qPrintable(parseFailure(single)));

        // Each capture candidate names itself `sharesrc`: handleBusMessage
        // recognises a device that will not open by that name.
        const QByteArray src = SOURCE_UNDER_TEST;
        QVERIFY2(!src.isEmpty(), "engine source unreadable");
        for (const char *element :
             { "wasapi2src name=sharesrc loopback-mode=exclude-process-tree",
               "wasapi2src name=sharesrc loopback=true",
               "wasapisrc name=sharesrc loopback=true",
               "pulsesrc name=sharesrc device=@DEFAULT_MONITOR@" }) {
            QVERIFY2(src.contains(element),
                     qPrintable(QStringLiteral(
                         "a share-audio capture candidate no longer names "
                         "itself `sharesrc`: %1").arg(element)));
        }
    }

    // The per-application track in every shape production now composes:
    // with the level meter, with branches built for a pipewiresrc that has no
    // `on-disconnect` (PipeWire < 1.6), and with a muted branch. Parsed for
    // real (§16: a lookalike composition proved nothing once already).
    void theChosenApplicationsTrackParsesInEveryShape()
    {
        for (const char *needed : { "audiomixer", "audiotestsrc", "audioconvert",
                                    "audioresample", "capsfilter", "valve",
                                    "opusenc", "rtpopuspay", "pipewiresrc",
                                    "volume", "level" }) {
            GstElementFactory *factory = gst_element_factory_find(needed);
            if (!factory)
                QSKIP(qPrintable(QStringLiteral("no %1 in this build")
                                     .arg(QLatin1String(needed))));
            gst_object_unref(factory);
        }
        const auto parseFailure = [](const QString &description) {
            GError *error = nullptr;
            GstElement *bin = gst_parse_bin_from_description(
                description.toUtf8().constData(), TRUE, &error);
            QString message =
                error && error->message ? QString::fromUtf8(error->message)
                                        : QString();
            if (error)
                g_error_free(error);
            // The level meter and every branch's volume must be findable by
            // the names the engine looks them up by.
            if (message.isEmpty() && bin) {
                GstElement *level = gst_bin_get_by_name(
                    GST_BIN(bin), "sharelevel");
                if (!level)
                    message = QStringLiteral("no element named sharelevel");
                else
                    gst_object_unref(level);
            }
            if (bin)
                gst_object_unref(bin);
            return message;
        };
        lightning::shareaudio::Stream a;
        a.serial = QStringLiteral("9551");
        lightning::shareaudio::Stream b;
        b.serial = QStringLiteral("307");
        for (bool retire : { true, false }) {
            lightning::shareaudio::BranchOptions options;
            options.retireOnDisconnect = retire;
            const QString whole = lightning::shareaudio::encodedTrackDescription(
                lightning::shareaudio::mixedSourceDescription({ a, b }, options),
                0x1234u, /*withLevel=*/true);
            const QString message = parseFailure(whole);
            QVERIFY2(message.isEmpty(),
                     qPrintable(QStringLiteral(
                         "the chosen-applications track (on-disconnect=%1) "
                         "does not parse: %2").arg(retire).arg(message)));
            QVERIFY(whole.contains(QStringLiteral("shareappvol1")));
        }

        // A branch added mid-share (rescan), muted from birth, parses alone.
        lightning::shareaudio::BranchOptions legacy;
        legacy.retireOnDisconnect = false;
        GError *error = nullptr;
        GstElement *branch = gst_parse_bin_from_description(
            lightning::shareaudio::applicationBranchDescription(a, 5, legacy,
                                                                true)
                .toUtf8()
                .constData(),
            TRUE, &error);
        const QString branchMessage =
            error && error->message ? QString::fromUtf8(error->message)
                                    : QString();
        if (error)
            g_error_free(error);
        QVERIFY2(branchMessage.isEmpty(), qPrintable(branchMessage));
        QVERIFY(branch);
        GstElement *volume = gst_bin_get_by_name(GST_BIN(branch), "shareappvol5");
        QVERIFY2(volume, "the branch's volume is not where a mute looks for it");
        gboolean mute = FALSE;
        g_object_get(volume, "mute", &mute, nullptr);
        QVERIFY(mute);
        gst_object_unref(volume);
        gst_object_unref(branch);
    }

    // Nothing running: a new choice is recorded for the next publish.
    void aShareAudioSelectionWithoutARunningShareIsOnlyRecorded()
    {
        SfuMediaEngine engine;
        lightning::shareaudio::Selection apps;
        apps.mode = lightning::shareaudio::Mode::Apps;
        apps.keys = { QStringLiteral("firefox") };
        QVERIFY(engine.applyShareAudioSelection(apps));
        // With nothing chosen and no peer, publishing adds no bin.
        lightning::shareaudio::Selection none;
        none.mode = lightning::shareaudio::Mode::Apps;
        engine.publishShareAudio(QStringLiteral("cid-none"), none);
        QVERIFY(!engine.hasPublishedBinForTest(QStringLiteral("cid-none")));
    }

    // ── A running per-application share, through the test seam ──
    //
    // The listing is scripted and the branches are audiotestsrc, so mutes,
    // retirements, the taking-out of a gone branch, re-adds and the cap run
    // on a real publisher pipeline without PipeWire or WASAPI (helpers:
    // ScriptedListing, above the class).

    // PipeWire policy: chosen apps get branches, a deselected one is muted,
    // a gone one is retired and then TAKEN OUT (so churn cannot use up the
    // cap), and the report says what is carried.
    void aPerApplicationShareFollowsTheChoiceAndTheStreams()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("11"), QStringLiteral("a")),
                          scriptedStream(QStringLiteral("12"), QStringLiteral("b")) };
        installListing(engine, listing, /*followsProcess=*/false);
        QSignalSpy reports(&engine, &SfuMediaEngine::shareAudioReport);
        engine.start();
        lightning::shareaudio::Selection apps;
        apps.mode = Mode::Apps;
        apps.keys = { QStringLiteral("a") };
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), apps);
        QTest::qWait(300);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-share-audio")));
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("11")) >= 0);
        QCOMPARE(engine.shareAudioBranchIndexForTest(QStringLiteral("12")), -2);
        QVERIFY(!reports.isEmpty());
        QCOMPARE(reports.last().at(3).toStringList(),
                 QStringList{ QStringLiteral("a") });
        QVERIFY(reports.last().at(1).toBool());   // per application

        // The whole system, in place: b gets a branch, a stays audible.
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        QVERIFY(engine.applyShareAudioSelection(system));
        QTest::qWait(200);
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);
        QVERIFY(!engine.shareAudioBranchMutedForTest(QStringLiteral("11")));

        // Only b: a is muted, never unlinked.
        lightning::shareaudio::Selection onlyB;
        onlyB.mode = Mode::Apps;
        onlyB.keys = { QStringLiteral("b") };
        QVERIFY(engine.applyShareAudioSelection(onlyB));
        QVERIFY(engine.shareAudioBranchMutedForTest(QStringLiteral("11")));
        QVERIFY(!engine.shareAudioBranchMutedForTest(QStringLiteral("12")));
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);

        // a's stream ends: retired on this scan, taken out on the next.
        listing->live = { scriptedStream(QStringLiteral("12"), QStringLiteral("b")) };
        engine.rescanShareAudioSourcesForTest();
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("11")) >= 0);
        QTest::qWait(300);
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(QStringLiteral("11")), -2);
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 1, 3000);
        QCOMPARE(engine.shareAudioBranchCountForTest(), 1);
        QTest::qWait(200);
        // The track still runs: the live publisher kept its share bin.
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-share-audio")));
        engine.stop();
    }

    // H1: with the Windows policy a branch follows its PROCESS. A session
    // that expires keeps its branch; the process exiting retires it; a
    // process that comes back is captured again.
    void aWindowsStyleBranchSurvivesItsSessionAndIsReAddedAfterItsProcess()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        const auto browser =
            scriptedStream(QStringLiteral("21"), QStringLiteral("chrome.exe"), 5);
        listing->live = { browser };
        listing->running = { browser.id() };
        installListing(engine, listing, /*followsProcess=*/true);
        engine.start();
        lightning::shareaudio::Selection apps;
        apps.mode = Mode::Apps;
        apps.keys = { QStringLiteral("chrome.exe") };
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), apps);
        QTest::qWait(300);
        const int first = engine.shareAudioBranchIndexForTest(browser.id());
        QVERIFY(first >= 0);

        // Paused: the session is gone from the listing, the process runs.
        listing->live = {};
        engine.rescanShareAudioSourcesForTest();
        QTest::qWait(200);
        engine.rescanShareAudioSourcesForTest();
        QVERIFY2(engine.shareAudioBranchIndexForTest(browser.id()) == first,
                 "an expired session cost its process the capture; it would "
                 "never be captured again");
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);

        // Playing again: the same branch, no second one.
        listing->live = { browser };
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(browser.id()), first);
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);

        // The process exits: retired, then taken out and forgotten.
        listing->live = {};
        listing->running = {};
        engine.rescanShareAudioSourcesForTest();
        QTest::qWait(300);
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(browser.id()), -2);
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 0, 3000);

        // It comes back: captured again, as a new branch.
        listing->live = { browser };
        listing->running = { browser.id() };
        engine.rescanShareAudioSourcesForTest();
        const int again = engine.shareAudioBranchIndexForTest(browser.id());
        QVERIFY2(again >= 0 && again != first,
                 "a returning application was never captured again");
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);
        engine.stop();
    }

    // M3: the cap counts what stands; a reclaimed branch frees its place and
    // the limit is reported, not silent.
    void theBranchCapIsReportedAndFreedByReclaiming()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setShareAudioBranchCapForTest(2);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("31"), QStringLiteral("a")),
                          scriptedStream(QStringLiteral("32"), QStringLiteral("b")),
                          scriptedStream(QStringLiteral("33"), QStringLiteral("c")) };
        installListing(engine, listing, /*followsProcess=*/false);
        QSignalSpy reports(&engine, &SfuMediaEngine::shareAudioReport);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);
        QVERIFY(!reports.isEmpty());
        QVERIFY2(reports.last().at(5).toBool(),
                 "the branch limit left an application out silently");

        listing->live.removeFirst();   // a leaves
        engine.rescanShareAudioSourcesForTest();
        QTest::qWait(300);
        engine.rescanShareAudioSourcesForTest();   // taken out; c added
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 2, 3000);
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("33")) >= 0);
        QVERIFY(!reports.last().at(5).toBool());
        engine.stop();
    }

    // A branch that ends by itself (pipewiresrc 1.6's on-disconnect=eos when
    // the daemon restarts) is retired even while the listing still shows its
    // id: a restarted daemon can give the SAME serial to the same
    // application's new stream, and keeping the ended branch for it carried
    // nothing for the rest of the share. It is taken out and the stream
    // captured again.
    void aBranchThatEndsByItselfIsReplacedEvenIfItsIdComesBack()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("37"), QStringLiteral("toneb")) };
        installListing(engine, listing, /*followsProcess=*/false);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        const int first = engine.shareAudioBranchIndexForTest(QStringLiteral("37"));
        QVERIFY(first >= 0);

        // The source ends on its own; the listing is unchanged.
        engine.sendShareAudioEosForTest(QStringLiteral("37"));
        QTRY_VERIFY_WITH_TIMEOUT(
            engine.shareAudioEosSeenForTest(QStringLiteral("37")), 3000);
        engine.rescanShareAudioSourcesForTest();   // retired, taken out
        engine.rescanShareAudioSourcesForTest();   // captured again
        const int again = engine.shareAudioBranchIndexForTest(QStringLiteral("37"));
        QVERIFY2(again >= 0 && again != first,
                 "an ended branch was kept for a live stream: silence");
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 1, 3000);
        engine.stop();
    }

    // N2, measured live: after `restart pipewire` under a running share, an
    // application came back at the SAME serial, and its old branch, whose
    // connection the daemon had closed, was kept for it. pipewiresrc neither
    // errors nor sends EOS when the daemon goes (its stream merely becomes
    // unconnected), so it carried silence for the rest of the share. A
    // branch whose own connection hung up is retired and the stream captured
    // again.
    void aBranchWhoseDaemonHungUpIsReplaced()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("37"), QStringLiteral("toneb")) };
        installListing(engine, listing, /*followsProcess=*/false);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        const int first = engine.shareAudioBranchIndexForTest(QStringLiteral("37"));
        QVERIFY(first >= 0);
        int pair[2] = { -1, -1 };
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
        engine.setShareAudioConnectionForTest(QStringLiteral("37"), pair[0]);

        // Connection open: nothing changes, however many scans.
        engine.rescanShareAudioSourcesForTest();
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(QStringLiteral("37")), first);

        // The daemon hangs up; the listing still shows serial 37.
        ::close(pair[1]);
        // Retired and taken out at once: its source can produce nothing more,
        // and after a real restart its EOS is never seen (a flush-start
        // pipewiresrc never ended drops it), so none is let through here.
        engine.setShareAudioSuppressRetireEosForTest(true);
        engine.rescanShareAudioSourcesForTest();
        // ...and in the same scan the stream is captured again.
        const int again = engine.shareAudioBranchIndexForTest(QStringLiteral("37"));
        QVERIFY2(again >= 0 && again != first,
                 "a branch whose daemon hung up was kept for the stream that "
                 "came back at its serial: silence");
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 1, 3000);
        engine.stop();
        ::close(pair[0]);
#else
        QSKIP("PipeWire is Linux-only");
#endif
    }

    // F3 (Flatpak RC): every video frame was its own queued event to the GUI
    // thread, so a GUI stall collected every frame it missed — 900 frames of
    // a 1080p share's self-view in a 30 s stall, RSS 1.1 -> 7.5 GB, host
    // down. Now a stall leaves ONE frame per key and one delivery.
    void aGuiStallKeepsOneVideoFrameNotEveryFrame()
    {
        SfuMediaEngine engine;
        SfuVideoRouter router;
        engine.setVideoRouter(&router);
        engine.setTestSourceMode(true);
        engine.setSelfViewInTestModeForTest(true);
        auto sink = std::make_unique<QVideoSink>();
        router.attachSink(SfuMediaEngine::localCameraStreamId(), sink.get());
        QSignalSpy frames(sink.get(), &QVideoSink::videoFrameChanged);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-video"), false, -1);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= 3, 10000);

        // The GUI thread stalls; the pipeline keeps producing.
        QThread::msleep(1500);
        QVERIFY2(router.pendingFramesForTest() <= 1,
                 "frames piled up for a stalled GUI thread");
        frames.clear();
        // Everything queued during the stall is delivered now, in one go.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const int burst = frames.count();
        QVERIFY2(burst <= 2,
                 qPrintable(QStringLiteral("%1 frames delivered at once after "
                                           "a 1.5 s stall; each was held in "
                                           "memory meanwhile")
                                .arg(burst)));
        // And the picture keeps moving afterwards.
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= burst + 3, 5000);
        engine.stop();
        engine.setVideoRouter(nullptr);
    }

    // F1 (Flatpak RC): pipewiresrc's start waits up to 30 s for its stream,
    // and right after a PipeWire restart it did — on the GUI thread, freezing
    // the app (and its call's delayed-leave refresh) for 30 s. A branch now
    // starts on a pool thread; the scan returns at once.
    void aSlowBranchStartDoesNotBlockTheGuiThread()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("51"), QStringLiteral("toneb")) };
        installListing(engine, listing, /*followsProcess=*/false);
        engine.setShareAudioStartDelayForTest(1500);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        QElapsedTimer clock;
        clock.start();
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        const qint64 blocked = clock.elapsed();
        QVERIFY2(blocked < 1000,
                 qPrintable(QStringLiteral("the GUI thread waited %1 ms for a "
                                           "branch to start")
                                .arg(blocked)));
        QVERIFY(engine.shareAudioBranchStartingForTest(QStringLiteral("51")));
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("51")) >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(
            !engine.shareAudioBranchStartingForTest(QStringLiteral("51")), 5000);
        QCOMPARE(engine.shareAudioCarriedForTest(),
                 QStringList{ QStringLiteral("toneb") });
        engine.stop();
    }

    // F1: a branch that would not start was recorded as failed and NEVER
    // retried: after one restart the application stayed out of the share
    // until the user unticked and ticked it. It is retried after a short
    // cool-down, and once captured it counts as carried again (the share had
    // logged "carrying= 0 app(s)" while it carried audio).
    void aBranchThatWouldNotStartIsRetried()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("52"), QStringLiteral("tonea")) };
        installListing(engine, listing, /*followsProcess=*/false);
        engine.failShareAudioStartsForTest(1);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioFailedKeysForTest(),
                                  QStringList{ QStringLiteral("tonea") }, 5000);
        QVERIFY(engine.shareAudioCarriedForTest().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 0, 3000);
        // A few scans later (the first cool-down is two), it is captured.
        for (int i = 0; i < 4; ++i)
            engine.rescanShareAudioSourcesForTest();
        QTRY_VERIFY_WITH_TIMEOUT(
            engine.shareAudioBranchIndexForTest(QStringLiteral("52")) >= 0
                && !engine.shareAudioBranchStartingForTest(QStringLiteral("52")),
            5000);
        QCOMPARE(engine.shareAudioCarriedForTest(),
                 QStringList{ QStringLiteral("tonea") });
        QVERIFY(engine.shareAudioFailedKeysForTest().isEmpty());
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);
        engine.stop();
    }

    // LOW (Flatpak RC): three PipeWire restarts within a minute cost two
    // attempts each, the budget of five reset only after 60 s without a
    // failure, and the microphone stayed dead for the rest of the call. A
    // restart that then delivers for 10 s worked; it stops counting.
    void aRestartThatDeliversNoLongerCountsAgainstTheMicBudget()
    {
        // Nothing to reset.
        QVERIFY(!SfuMediaEngine::micRestartBudgetResets(0, 5000, true));
        // Failing again soon, having delivered nothing: the budget runs on.
        QVERIFY(!SfuMediaEngine::micRestartBudgetResets(3, 15000, false));
        // Delivered, but only just restarted: not yet proven.
        QVERIFY(!SfuMediaEngine::micRestartBudgetResets(3, 5000, true));
        // Delivered and ran 10 s: that restart worked (muted or not, since
        // it is counted at the source).
        QVERIFY(SfuMediaEngine::micRestartBudgetResets(3, 10000, true));
        // A minute without a failure, the old rule.
        QVERIFY(SfuMediaEngine::micRestartBudgetResets(5, 60001, false));
    }

    // F2 (Flatpak RC, then measured here on WirePlumber 0.5.14): a capture
    // whose target is missing was linked to the DEFAULT SOURCE — the
    // microphone — so an application read "In the share" while the share
    // carried the mic (silence, in that rig). The branch's stream must forbid
    // fallback, reconnection and moves, as parsed for real.
    void thePipeWireBranchNeverFallsBackToAnotherNode()
    {
        if (GstElementFactory *f = gst_element_factory_find("pipewiresrc"))
            gst_object_unref(f);
        else
            QSKIP("no pipewiresrc in this build");
        lightning::shareaudio::Stream s;
        s.serial = QStringLiteral("4242");
        lightning::shareaudio::BranchOptions options;
        options.capture = lightning::shareaudio::Capture::PipeWire;
        const QString description =
            lightning::shareaudio::applicationBranchDescription(s, 7, options);
        GError *error = nullptr;
        GstElement *branch = gst_parse_bin_from_description(
            description.toUtf8().constData(), TRUE, &error);
        QVERIFY2(!error, error ? error->message : "");
        QVERIFY(branch);
        GstElement *source = gst_bin_get_by_name(GST_BIN(branch), "shareapp7");
        QVERIFY(source);
        GstStructure *props = nullptr;
        g_object_get(source, "stream-properties", &props, nullptr);
        QVERIFY2(props, "no stream-properties on the capture");
        for (const char *field : { "node.dont-fallback", "node.dont-reconnect",
                                   "node.dont-move" }) {
            const GValue *value = gst_structure_get_value(props, field);
            QVERIFY2(value, field);
            gchar *text = gst_value_serialize(value);
            QCOMPARE(QString::fromUtf8(text), QStringLiteral("true"));
            g_free(text);
        }
        gst_structure_free(props);
        gst_object_unref(source);
        gst_object_unref(branch);
    }

    // N1: a retired branch is taken out only once its EOS has actually
    // passed its src pad. "Retired a scan ago" was the old rule, and a
    // selection change rescans at once: two quick changes took a branch out
    // milliseconds after its EOS was sent, while it was still pushing, with a
    // synchronous NULL on the GUI thread. Here the EOS is withheld, so the
    // branch goes on producing exactly as a source mid-push would.
    void aRetiredBranchIsNotTakenOutUntilItsEosHasPassed()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("41"), QStringLiteral("a")),
                          scriptedStream(QStringLiteral("42"), QStringLiteral("b")) };
        installListing(engine, listing, /*followsProcess=*/false);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);

        engine.setShareAudioSuppressRetireEosForTest(true);
        listing->live = { scriptedStream(QStringLiteral("42"), QStringLiteral("b")) };
        engine.rescanShareAudioSourcesForTest();          // retires a
        // Quick selection changes and scans, back to back, while a produces.
        lightning::shareaudio::Selection onlyB;
        onlyB.mode = Mode::Apps;
        onlyB.keys = { QStringLiteral("b") };
        QVERIFY(engine.applyShareAudioSelection(onlyB));
        QVERIFY(engine.applyShareAudioSelection(system));
        for (int i = 0; i < 5; ++i)
            engine.rescanShareAudioSourcesForTest();
        QTest::qWait(300);
        engine.rescanShareAudioSourcesForTest();
        QVERIFY(!engine.shareAudioEosSeenForTest(QStringLiteral("41")));
        QVERIFY2(engine.shareAudioBranchIndexForTest(QStringLiteral("41")) >= 0,
                 "a branch was taken out before its EOS had passed — while it "
                 "could still be pushing");
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);

        // Its EOS passes: now it is taken out (asynchronously), and the share
        // runs on.
        engine.setShareAudioSuppressRetireEosForTest(false);
        engine.sendShareAudioEosForTest(QStringLiteral("41"));
        QTRY_VERIFY_WITH_TIMEOUT(
            engine.shareAudioEosSeenForTest(QStringLiteral("41")), 3000);
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(QStringLiteral("41")), -2);
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 1, 3000);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-share-audio")));
        engine.stop();
    }

    /// A key that reached nobody is installed but not adopted for encryption;
    /// otherwise every later frame is encrypted under a key no peer has.
    void anUndeliveredKeyIsInstalledButNotAdopted()
    {
        SfuMediaEngine engine;
        // The first key of a call is undeliverable (nobody else has joined)
        // and must still be adopted.
        engine.setOutboundKey(1, QByteArray(32, 'a'));
        QCOMPARE(engine.adoptedOutboundKeyIndexForTest(), 1);

        // A rotation that reached nobody while a peer holds index 1: installed
        // in the ring, but we keep sending under index 1.
        engine.setOutboundKey(2, QByteArray(32, 'b'), /*adopt=*/false);
        QCOMPARE(engine.adoptedOutboundKeyIndexForTest(), 1);

        // Once a distribution reaches someone, we move.
        engine.setOutboundKey(3, QByteArray(32, 'c'), /*adopt=*/true);
        QCOMPARE(engine.adoptedOutboundKeyIndexForTest(), 3);
    }

    // livekit-server injects unencrypted Opus silence frames ending in the
    // room's `sif_trailer` when a publisher mutes. They are dropped apart,
    // not counted as decryption failures and never fed to the badge window.
    void aServerInjectedBurstIsDroppedApartAndNeverBadges()
    {
        SfuMediaEngine engine;
        const QString stream = QStringLiteral("PA_sif");
        const QByteArray key(16, 'e');
        engine.setEncryptionRequired(true);
        engine.setInboundKey(stream, 82, key);
        engine.setServerInjectedTrailer(roomTrailer());
        QCOMPARE(engine.serverInjectedTrailer(), roomTrailer());

        // Real frames under index 82 also end in 'R', so only an exact trailer
        // match separates them.
        CallFrameCryptor sender;
        QVERIFY(sender.setKey(82, key));
        sender.setCurrentKeyIndex(82);

        QSignalSpy blocked(&engine, &SfuMediaEngine::remoteMediaBlocked);
        LogCapture log;
        DecryptProbeRig rig(engine, stream);
        QVERIFY(rig.ok());
        for (int i = 0; i < 5; ++i) {
            rig.push(sender.encryptFrame(
                QByteArray("\x01") + QByteArray(40, char('a' + i)),
                CallFrameCryptor::FrameKind::Audio, 9,
                static_cast<quint32>(960 * i)));
        }
        for (int i = 0; i < 50; ++i)
            rig.push(opusSilenceFrame() + roomTrailer());
        QVERIFY(rig.drain());

        QCOMPARE(engine.framesServerInjected(), quint64(50));
        QCOMPARE(engine.framesDecrypted(), quint64(5));
        QVERIFY2(engine.framesDropped() == 0,
                 qPrintable(QStringLiteral("server-injected frames were "
                                           "counted as drops: %1\n%2")
                                .arg(engine.framesDropped())
                                .arg(log.text())));
        // Dropped, not handed to the decoder: the SFU is outside the trust
        // boundary.
        QCOMPARE(rig.passed(), 5);
        QVERIFY2(!log.contains("will not DECRYPT"),
                 qPrintable(log.text()));
        QVERIFY(log.contains("SERVER-INJECTED"));
        // The badge verdict is queued to the engine's thread.
        QTest::qWait(100);
        for (const QList<QVariant> &call : blocked) {
            QVERIFY2(call.at(1).toString().isEmpty(),
                     qPrintable(QStringLiteral(
                         "a mute's injected frames raised the badge: %1")
                                    .arg(call.at(1).toString())));
        }
        // Positive control: the harness does see a raise when frames really
        // fail.
        {
            QSignalSpy control(&engine, &SfuMediaEngine::remoteMediaBlocked);
            DecryptProbeRig failing(engine, stream);
            QVERIFY(failing.ok());
            for (int i = 0; i < 60; ++i)
                failing.push(QByteArray(60, char('a' + i % 20)));
            QVERIFY(failing.drain());
            QTRY_VERIFY_WITH_TIMEOUT(control.count() > 0, 2000);
            QCOMPARE(control.first().at(1).toString(),
                     QStringLiteral("undecryptable"));
        }

        // The burst summary is logged when the stream ends, since a muted
        // sender sends nothing afterwards.
        rig.finish();
        QVERIFY2(log.contains("server-injected burst")
                     && log.text().contains(QRegularExpression(
                         QStringLiteral("frames= 50\\b"))),
                 qPrintable(log.text()));
        QVERIFY2(log.text().contains(QRegularExpression(
                     QStringLiteral("decrypt histogram final.*sif= 50"))),
                 qPrintable(log.text()));
    }

    // Control: the same frames with no trailer armed still fail as
    // bad-iv-length.
    void withoutATrailerTheSameFramesStillFailAsBadIvLength()
    {
        SfuMediaEngine engine;
        const QString stream = QStringLiteral("PA_nosif");
        engine.setEncryptionRequired(true);
        engine.setInboundKey(stream, 0, QByteArray(16, 'e'));
        QVERIFY(engine.serverInjectedTrailer().isEmpty());

        LogCapture log;
        DecryptProbeRig rig(engine, stream);
        QVERIFY(rig.ok());
        for (int i = 0; i < 50; ++i)
            rig.push(opusSilenceFrame() + roomTrailer());
        QVERIFY(rig.drain());

        QCOMPARE(engine.framesServerInjected(), quint64(0));
        QCOMPARE(engine.framesDropped(), quint64(50));
        QCOMPARE(rig.passed(), 0);
        QVERIFY2(log.contains("will not DECRYPT: reason= bad-iv-length "
                              "keyIndex= 82"),
                 qPrintable(log.text()));
        // The detail line names the LiveKit silence payload, a base62 tail and
        // an unarmed trailer.
        QVERIFY2(log.text().contains(QRegularExpression(QStringLiteral(
                     "decrypt failed detail .*reason= bad-iv-length size= 123 "
                     "ivLenByte= \\d+ keyIndexByte= 82 .*silenceShape= true "
                     "tailBase62= true sifArmed= false"))),
                 qPrintable(log.text()));
        // Bounded: five detail lines for fifty failures of one reason, plus
        // the shouldReport() count at 10.
        QCOMPARE(log.count("decrypt failed detail"), 6);
    }

    // An oversized or absent trailer disarms; the trailer is cleared with the
    // call's keys.
    void theTrailerIsBoundedAndClearedWithTheKeys()
    {
        SfuMediaEngine engine;
        engine.setServerInjectedTrailer(roomTrailer());
        QCOMPARE(engine.serverInjectedTrailer(), roomTrailer());
        engine.setServerInjectedTrailer(
            QByteArray(CallFrameCryptor::kMaxServerTrailerBytes + 1, 'A'));
        QVERIFY2(engine.serverInjectedTrailer().isEmpty(),
                 "an oversized trailer was armed (or kept the old one)");
        // Only LiveKit's shape arms: >= 16 base62 bytes.
        const QByteArray notArming[] = {
            QByteArray("R"), QByteArray("\x0c\x52", 2),
            QByteArray(CallFrameCryptor::kMinServerTrailerBytes - 1, 'A'),
            QByteArray("k3P9dQ2mZ7xW4vB8+T1cY6hJ0fL5sG2aE9rU3oKqXiR"),
        };
        for (const QByteArray &bad : notArming) {
            engine.setServerInjectedTrailer(roomTrailer());
            engine.setServerInjectedTrailer(bad);
            QVERIFY2(engine.serverInjectedTrailer().isEmpty(),
                     qPrintable(QStringLiteral("armed with %1 bytes: %2")
                                    .arg(bad.size())
                                    .arg(QString::fromLatin1(bad.toHex()))));
        }
        engine.setServerInjectedTrailer(
            QByteArray(CallFrameCryptor::kMinServerTrailerBytes, 'A'));
        QCOMPARE(engine.serverInjectedTrailer().size(),
                 CallFrameCryptor::kMinServerTrailerBytes);
        engine.setServerInjectedTrailer(roomTrailer());
        engine.clearKeys();
        QVERIFY2(engine.serverInjectedTrailer().isEmpty(),
                 "a trailer outlived the keys of the call that armed it");
        engine.setServerInjectedTrailer(roomTrailer());
        engine.stop();
        QVERIFY(engine.serverInjectedTrailer().isEmpty());
    }

    // One diagnosis line per failure reason, not per stream.
    void eachFailureReasonGetsItsOwnDiagnosisLine()
    {
        SfuMediaEngine engine;
        const QString stream = QStringLiteral("PA_reasons");
        engine.setEncryptionRequired(true);
        engine.setInboundKey(stream, 0, QByteArray(16, 'e'));

        CallFrameCryptor otherIndex;
        QVERIFY(otherIndex.setKey(5, QByteArray(16, 'e')));
        otherIndex.setCurrentKeyIndex(5);

        LogCapture log;
        DecryptProbeRig rig(engine, stream);
        QVERIFY(rig.ok());
        // bad-iv-length first (a frame with no crypto trailer at all)...
        rig.push(QByteArray(60, 'z'));
        rig.push(QByteArray(60, 'y'));
        // ...then a well-formed frame under an index this ring never got.
        rig.push(otherIndex.encryptFrame(QByteArray("\x01") + QByteArray(30, 'q'),
                                         CallFrameCryptor::FrameKind::Audio,
                                         3, 960));
        QVERIFY(rig.drain());

        QCOMPARE(log.count("will not DECRYPT: reason= bad-iv-length"), 1);
        QVERIFY2(log.contains("will not DECRYPT: reason= no-key-for-index "
                              "keyIndex= 5"),
                 qPrintable(log.text()));
    }

    // Key arrivals have their own bounded log and do not use up the shared
    // once-per-subject diagnosis set (512 entries).
    void keyArrivalsDoNotSilenceOtherDiagnoses()
    {
        LogCapture log;
        SfuMediaEngine engine;
        for (int ring = 0; ring < 3; ++ring) {
            for (int index = 0; index < 200; ++index) {
                engine.setInboundKey(QStringLiteral("@r%1:x/DEV").arg(ring),
                                     index, QByteArray(16, char('a' + ring)));
            }
        }
        // Bounded: the first 8 per ring, then every 16th.
        const int arrived = log.count("media key ARRIVED");
        QVERIFY2(arrived >= 3 && arrived <= 3 * (8 + 200 / 16),
                 qPrintable(QString::number(arrived)));
        engine.noteParticipantIdentity(QString(),
                                       QStringLiteral("@a:example.org:DEV"));
        QVERIFY2(log.contains("could NOT be bound to a sending stream"),
                 "key arrivals used up the diagnosis set");
    }

    // An Element peer's key at index 200 decrypts its frames end to end
    // through the engine's ring and the real probe.
    void aSenderKeyedAtIndex200IsDecryptedThroughTheProbe()
    {
        SfuMediaEngine engine;
        const QString stream = QStringLiteral("PA_rotated");
        const QByteArray key(16, 'r');
        engine.setEncryptionRequired(true);
        LogCapture log;
        engine.setInboundKey(stream, 200, key);
        QVERIFY2(!log.contains("REFUSED by the cryptor"),
                 qPrintable(log.text()));
        QVERIFY(engine.recvCryptorFor(stream)->hasKey(200));

        CallFrameCryptor sender;
        QVERIFY(sender.setKey(200, key));
        sender.setCurrentKeyIndex(200);
        DecryptProbeRig rig(engine, stream);
        QVERIFY(rig.ok());
        for (int i = 0; i < 10; ++i) {
            rig.push(sender.encryptFrame(
                QByteArray("\x01") + QByteArray(30, char('0' + i)),
                CallFrameCryptor::FrameKind::Audio, 4,
                static_cast<quint32>(960 * i)));
        }
        QVERIFY(rig.drain());
        QCOMPARE(engine.framesDecrypted(), quint64(10));
        QCOMPARE(engine.framesDropped(), quint64(0));
        QCOMPARE(rig.passed(), 10);
        // An index past the ring is still refused.
        engine.setInboundKey(stream, 256, key);
        QVERIFY(log.contains("REFUSED by the cryptor"));
    }

    // One sender with no key must not silence the others. Its frames are all
    // dropped, so its sink never prerolls; before the fix that held the whole
    // subscriber pipeline in PAUSED and every bin added later prerolled one
    // frame and stopped, until the missing key arrived. Measured live
    // 2026-09-25. The bins are the engine's own (buildReceiveBin); two live
    // Opus RTP sources behind a jitterbuffer stand in for webrtcbin's pads.
    void aTrackThatNeverDeliversDoesNotSilenceTheOthers()
    {
        struct Counter { std::atomic<int> n{0}; };
        Counter keyed;
        Counter keyless;
        // What cryptoProbe returns for a sender with no key: DROP. Flipped
        // later to stand for the key arriving.
        std::atomic<bool> keyArrived{false};

        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "audiotestsrc is-live=true freq=500 ! opusenc ! rtpopuspay "
            "! rtpjitterbuffer latency=100 ! tee name=ta allow-not-linked=true "
            "audiotestsrc is-live=true freq=700 ! opusenc ! rtpopuspay "
            "! rtpjitterbuffer latency=100 ! tee name=tb allow-not-linked=true",
            &error);
        if (error) {
            const QString message = QString::fromUtf8(error->message);
            g_error_free(error);
            if (pipeline)
                gst_object_unref(pipeline);
            QFAIL(qPrintable(message));
        }
        QVERIFY(pipeline);
        // Stops the streaming threads before the counters above go away,
        // including on an early return.
        struct Stop {
            GstElement *pipeline;
            ~Stop()
            {
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
            }
        } stop{pipeline};
        QVERIFY(gst_element_set_state(pipeline, GST_STATE_PLAYING)
                != GST_STATE_CHANGE_FAILURE);
        QVERIFY(gst_element_get_state(pipeline, nullptr, nullptr,
                                      5 * GST_SECOND)
                != GST_STATE_CHANGE_FAILURE);

        // Counts the buffers reaching the bin's sink; returns the sink (a
        // pointer only, owned by the bin) or null. The test sink is made to
        // sync like the autoaudiosink it stands in for: only a syncing sink
        // is given the upstream latency.
        const auto countAtSink = [](GstElement *bin,
                                    Counter *counter) -> GstElement * {
            GstIterator *it = gst_bin_iterate_sinks(GST_BIN(bin));
            GValue item = G_VALUE_INIT;
            GstElement *found = nullptr;
            if (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
                found = GST_ELEMENT(g_value_get_object(&item));
                g_object_set(found, "sync", TRUE, nullptr);
                GstPad *pad = gst_element_get_static_pad(found, "sink");
                gst_pad_add_probe(
                    pad, GST_PAD_PROBE_TYPE_BUFFER,
                    [](GstPad *, GstPadProbeInfo *, gpointer data) {
                        static_cast<Counter *>(data)->n.fetch_add(1);
                        return GST_PAD_PROBE_OK;
                    },
                    counter, nullptr);
                gst_object_unref(pad);
            }
            g_value_unset(&item);
            gst_iterator_free(it);
            return found;
        };
        // Added as soon as it is built, so the pipeline owns it before any
        // check can return early; then synced and fed, as onPadAdded does.
        const auto adopt = [pipeline](GstElement *bin) {
            return bin && gst_bin_add(GST_BIN(pipeline), bin);
        };
        const auto syncAndLink = [pipeline](GstElement *bin,
                                            const char *teeName) {
            gst_element_sync_state_with_parent(bin);
            if (!teeName)
                return true; // added, never fed
            GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), teeName);
            GstPad *src = gst_element_request_pad_simple(tee, "src_%u");
            GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
            const bool linked = gst_pad_link(src, sinkPad) == GST_PAD_LINK_OK;
            gst_object_unref(sinkPad);
            gst_object_unref(src);
            gst_object_unref(tee);
            return linked;
        };

        // The key-less sender first, then a video track that never delivers.
        QString why;
        GstElement *noKey = SfuMediaEngine::buildReceiveBin(
            false, QStringLiteral("outvol_nokey"), true, &why);
        QVERIFY2(adopt(noKey), qPrintable(why));
        GstElement *depay = gst_bin_get_by_name(GST_BIN(noKey), "recvdepay");
        QVERIFY(depay);
        GstPad *depaySrc = gst_element_get_static_pad(depay, "src");
        gst_pad_add_probe(
            depaySrc, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                return static_cast<std::atomic<bool> *>(data)->load()
                    ? GST_PAD_PROBE_OK
                    : GST_PAD_PROBE_DROP;
            },
            &keyArrived, nullptr);
        gst_object_unref(depaySrc);
        gst_object_unref(depay);
        QVERIFY(countAtSink(noKey, &keyless));
        QVERIFY(syncAndLink(noKey, "tb"));

        GstElement *silentVideo = SfuMediaEngine::buildReceiveBin(
            true, QString(), true, &why);
        QVERIFY2(adopt(silentVideo), qPrintable(why));
        QVERIFY(syncAndLink(silentVideo, nullptr));
        QTest::qWait(300);

        // Then a sender whose frames decrypt.
        GstElement *withKey = SfuMediaEngine::buildReceiveBin(
            false, QStringLiteral("outvol_keyed"), true, &why);
        QVERIFY2(adopt(withKey), qPrintable(why));
        GstElement *keyedSink = countAtSink(withKey, &keyed);
        QVERIFY(keyedSink);
        QVERIFY(syncAndLink(withKey, "ta"));

        // Opus at 20 ms is ~50 frames a second. Stuck in preroll, the sink
        // takes exactly one.
        QTRY_VERIFY_WITH_TIMEOUT(keyed.n.load() > 10, 5000);
        const int before = keyed.n.load();
        QTest::qWait(1000);
        const int inOneSecond = keyed.n.load() - before;
        GstState state = GST_STATE_VOID_PENDING;
        gst_element_get_state(pipeline, &state, nullptr, 0);
        QVERIFY2(inOneSecond >= 20,
                 qPrintable(QStringLiteral(
                     "the keyed sender delivered %1 frames in one second "
                     "beside a key-less one (pipeline %2): one undecryptable "
                     "stream silences everyone")
                                .arg(inOneSecond)
                                .arg(QString::fromUtf8(
                                    gst_element_state_get_name(state)))));
        QCOMPARE(state, GST_STATE_PLAYING);
        QCOMPARE(keyless.n.load(), 0);
        // The pipeline no longer replays PAUSED to PLAYING for each new bin,
        // which is what used to give each sink the jitterbuffer's latency.
        // The bin must do it itself, or a syncing audio sink renders every
        // frame late.
        const GstClockTime latency =
            gst_base_sink_get_latency(GST_BASE_SINK(keyedSink));
        QVERIFY2(latency >= 100 * GST_MSECOND,
                 qPrintable(QStringLiteral("the receive sink was configured "
                                           "for %1 ms of latency behind a "
                                           "100 ms jitterbuffer")
                                .arg(latency / GST_MSECOND)));

        // The key arrives: that sender is heard too, and the other goes on.
        keyArrived.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(keyless.n.load() > 20, 5000);
        const int keyedNow = keyed.n.load();
        QTRY_VERIFY_WITH_TIMEOUT(keyed.n.load() > keyedNow + 10, 5000);
    }

    // The send side of the same stall. A camera publish bin whose capture
    // never delivers a frame left its self-view appsink waiting for a preroll
    // frame, which held the publisher pipeline in PAUSED; every bin published
    // after it synced to PAUSED and its live source produced nothing. The
    // camera bin is the real publish description with the real self-view
    // branch; a funnel into a sink stands in for webrtcbin, and a live Opus
    // source for the next publish.
    void aCameraThatNeverDeliversDoesNotStallLaterPublishes()
    {
        std::atomic<int> sent{0};
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "funnel name=wire ! fakesink name=out sync=false async=false",
            &error);
        if (error) {
            const QString message = QString::fromUtf8(error->message);
            g_error_free(error);
            if (pipeline)
                gst_object_unref(pipeline);
            QFAIL(qPrintable(message));
        }
        QVERIFY(pipeline);
        // Stops the streaming threads before `sent` goes away, including on
        // an early return.
        struct Stop {
            GstElement *pipeline;
            ~Stop()
            {
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
            }
        } stop{pipeline};
        GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
        QVERIFY(out);
        GstPad *outPad = gst_element_get_static_pad(out, "sink");
        gst_pad_add_probe(
            outPad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            },
            &sent, nullptr);
        gst_object_unref(outPad);
        gst_object_unref(out);
        QVERIFY(gst_element_set_state(pipeline, GST_STATE_PLAYING)
                != GST_STATE_CHANGE_FAILURE);
        QVERIFY(gst_element_get_state(pipeline, nullptr, nullptr,
                                      5 * GST_SECOND)
                != GST_STATE_CHANGE_FAILURE);

        // Added, linked, then synced: publishVideo()'s order.
        const auto publish = [pipeline](GstElement *bin) {
            if (!bin || !gst_bin_add(GST_BIN(pipeline), bin))
                return false;
            GstElement *wire = gst_bin_get_by_name(GST_BIN(pipeline), "wire");
            GstPad *sinkPad = gst_element_request_pad_simple(wire, "sink_%u");
            GstPad *srcPad = gst_element_get_static_pad(bin, "src");
            const bool linked =
                srcPad && gst_pad_link(srcPad, sinkPad) == GST_PAD_LINK_OK;
            if (srcPad)
                gst_object_unref(srcPad);
            gst_object_unref(sinkPad);
            gst_object_unref(wire);
            gst_element_sync_state_with_parent(bin);
            return linked;
        };

        // A camera that opens and never produces a frame.
        lightning::rtp::registerVp8Payloader();
        const QString camera = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("appsrc is-live=true format=time"),
            SfuMediaEngine::cameraRateStage(false),
            SfuMediaEngine::cameraLimitsCaps(false),
            QStringLiteral("vp8enc deadline=1"),
            SfuMediaEngine::selfViewBranch(), 4321u,
            QStringLiteral("videoconvert ! videoscale"),
            SfuMediaEngine::captureEntryFilter(false));
        GstElement *cameraBin = gst_parse_bin_from_description(
            camera.toUtf8().constData(), TRUE, &error);
        if (error) {
            const QString message = QString::fromUtf8(error->message);
            g_error_free(error);
            if (cameraBin)
                gst_object_unref(cameraBin);
            QFAIL(qPrintable(message));
        }
        QVERIFY(publish(cameraBin));
        QTest::qWait(300);

        // Then the next publish, which must flow.
        GstElement *next = gst_parse_bin_from_description(
            "audiotestsrc is-live=true ! opusenc ! rtpopuspay", TRUE, &error);
        if (error) {
            const QString message = QString::fromUtf8(error->message);
            g_error_free(error);
            if (next)
                gst_object_unref(next);
            QFAIL(qPrintable(message));
        }
        QVERIFY(publish(next));

        // Opus at 20 ms is ~50 packets a second; a bin synced to PAUSED sends
        // none.
        QTRY_VERIFY_WITH_TIMEOUT(sent.load() > 10, 5000);
        GstState state = GST_STATE_VOID_PENDING;
        gst_element_get_state(pipeline, &state, nullptr, 0);
        QCOMPARE(state, GST_STATE_PLAYING);
    }

    // 2026-10-07 report (Debian 12 Flatpak): "my camera registers, but
    // there's no feed". A camera whose capture opens and never delivers a
    // buffer (a portal PipeWire remote with no camera node linked to it)
    // posts no bus error, so the track stayed declared and black for the
    // whole call. It must be reported, once, as `camera_no_frames`. Fails on
    // the old engine, which never reports a silent camera.
    void aCameraThatNeverDeliversIsReportedAsNoFrames()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(
            QStringLiteral("appsrc is-live=true format=time"));
        engine.setCameraFirstFrameTimeoutForTest(1500);
        QSignalSpy trackFailed(&engine, &SfuMediaEngine::publishFailed);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-camera"),
                            /*screenShare=*/false, -1, -1);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-camera")));
        QTRY_COMPARE_WITH_TIMEOUT(trackFailed.count(), 1, 10000);
        QCOMPARE(trackFailed.at(0).at(0).toString(),
                 QStringLiteral("cid-camera"));
        QCOMPARE(trackFailed.at(0).at(1).toString(),
                 QStringLiteral("camera_no_frames"));
        // Once, and the call itself is not failed.
        QTest::qWait(3500);
        QCOMPARE(trackFailed.count(), 1);
        QCOMPARE(failed.count(), 0);
        engine.stop();
    }

    // 2026-10-07, Debian 12 Flatpak: a portal camera whose PipeWire link
    // never negotiated froze the whole app for 30 s (pipewiresrc's start
    // blocks the thread that starts it), long enough for the call's own
    // delayed leave to drop us. A portal camera (a descriptor is passed) is
    // started off the GUI thread now, and still starts. Fails on the old
    // engine, where publishVideo() returns only after the start.
    void aPortalCameraIsStartedOffTheGuiThread()
    {
        static const bool registered =
            gst_element_register(nullptr, "lightningslowstartcam",
                                 GST_RANK_NONE, slow_start_cam_get_type());
        QVERIFY(registered);
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(
            QStringLiteral("lightningslowstartcam"));
        engine.start();
        const QString cid = QStringLiteral("cid-portal-camera");
        // Stands in for the portal's PipeWire descriptor; the engine owns
        // and closes it.
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        QElapsedTimer took;
        took.start();
        engine.publishVideo(cid, /*screenShare=*/false, /*nodeId=*/-1, fd);
        const qint64 ms = took.elapsed();
        QVERIFY(engine.hasPublishedBinForTest(cid));
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("publishVideo() blocked %1 ms on "
                                           "the camera's start")
                                .arg(ms)));
        // And it starts: frames leave `capsrc`.
        std::atomic<int> frames{0};
        GstElement *bin = engine.publishedBinForTest(cid);
        QVERIFY(bin);
        GstElement *capture = gst_bin_get_by_name(GST_BIN(bin), "capsrc");
        QVERIFY(capture);
        GstPad *out = gst_element_get_static_pad(capture, "src");
        const gulong probe = gst_pad_add_probe(
            out, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            },
            &frames, nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(frames.load() > 5, 15000);
        gst_pad_remove_probe(out, probe);
        gst_object_unref(out);
        gst_object_unref(capture);
        engine.stop();
    }

    // Review of the first fix: moving the start off the GUI thread only moved
    // the freeze. A teardown while the start was still inside the camera
    // (hang-up, an ICE reconnect's suspend, quit) set the publisher pipeline
    // to NULL on the GUI thread, which waits for the bin's state lock the
    // start holds. The starting bin is detached and finished on a pool thread
    // now. Fails on the first fix: stop() and suspend() each take the whole
    // start (~2.5 s) there.
    void aTeardownDuringAPortalCameraStartDoesNotWaitForIt()
    {
        static const bool registered =
            gst_element_register(nullptr, "lightningslowstartcam",
                                 GST_RANK_NONE, slow_start_cam_get_type());
        QVERIFY(registered);
        for (const bool suspend : { false, true }) {
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.setVideoSourceForTest(
                QStringLiteral("lightningslowstartcam"));
            engine.start();
            const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            QVERIFY(fd >= 0);
            engine.publishVideo(QStringLiteral("cid-portal-camera"),
                                /*screenShare=*/false, /*nodeId=*/-1, fd);
            QVERIFY(engine.hasPublishedBinForTest(
                QStringLiteral("cid-portal-camera")));
            QElapsedTimer took;
            took.start();
            if (suspend)
                engine.suspend();
            else
                engine.stop();
            const qint64 ms = took.elapsed();
            QVERIFY2(ms < kSlowStartCamMs / 2,
                     qPrintable(QStringLiteral("%1() waited %2 ms for a "
                                               "camera still starting")
                                    .arg(suspend ? QStringLiteral("suspend")
                                                 : QStringLiteral("stop"))
                                    .arg(ms)));
            // Let the detached start finish before the next round.
            QTest::qWait(kSlowStartCamMs + 500);
        }
    }

    // Measured live on Debian 12 after the first round of this fix: a
    // hang-up 3 s after a portal camera started froze the app for 30 s even
    // though the start had RETURNED, because the wedged pipewiresrc blocks
    // the next state change too and teardown set the publisher to NULL on
    // the GUI thread. Every portal camera is detached at teardown now,
    // started or not. Fails when only a still-starting bin is detached:
    // stop() then takes the camera's whole stop (~2.5 s).
    void aTeardownNeverStopsAPortalCameraOnTheGuiThread()
    {
        static const bool registered =
            gst_element_register(nullptr, "lightningslowstopcam",
                                 GST_RANK_NONE, slow_stop_cam_get_type());
        QVERIFY(registered);
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(QStringLiteral("lightningslowstopcam"));
        engine.start();
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        const QString cid = QStringLiteral("cid-portal-camera");
        engine.publishVideo(cid, /*screenShare=*/false, /*nodeId=*/-1, fd);
        GstElement *bin = engine.publishedBinForTest(cid);
        QVERIFY(bin);
        // Started: the bin reaches PLAYING.
        QTRY_VERIFY_WITH_TIMEOUT(
            [bin] {
                GstState state = GST_STATE_VOID_PENDING;
                gst_element_get_state(bin, &state, nullptr, 0);
                return state == GST_STATE_PLAYING;
            }(),
            5000);
        QElapsedTimer took;
        took.start();
        engine.stop();
        const qint64 ms = took.elapsed();
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("stop() waited %1 ms on a portal "
                                           "camera's own stop")
                                .arg(ms)));
        // Let the detached bin's stop finish (two blocking transitions).
        QTest::qWait(2 * kSlowStartCamMs + 500);
    }

    // Review of camfix-2: the takeover is for PORTAL cameras only. A direct
    // camera (or a share) that never delivered has no ProbeGate, so it must
    // keep the counted teardown path that stop() and the destructor wait for;
    // taken over, its stop would run on an uncounted pool thread while its
    // probes still point into the engine. Fails when unpublish() takes over
    // every silent bin: the counted teardown never starts.
    void aSilentDirectCameraKeepsTheCountedTeardown()
    {
        static const bool registered =
            gst_element_register(nullptr, "lightningslowstopcam",
                                 GST_RANK_NONE, slow_stop_cam_get_type());
        QVERIFY(registered);
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(QStringLiteral("lightningslowstopcam"));
        engine.start();
        const QString cid = QStringLiteral("cid-direct-camera");
        engine.publishVideo(cid, /*screenShare=*/false, /*nodeId=*/-1,
                            /*pipewireFd=*/-1);
        QVERIFY(engine.hasPublishedBinForTest(cid));
        QCOMPARE(engine.pendingTeardownsForTest(), 0);
        engine.unpublish(cid);
        // The camera's stop blocks for ~2.5 s, so a counted teardown is
        // still outstanding here.
        QVERIFY2(engine.pendingTeardownsForTest() > 0,
                 "a direct camera's teardown was taken off the counted path");
        QTRY_COMPARE_WITH_TIMEOUT(engine.pendingTeardownsForTest(), 0,
                                  4 * kSlowStartCamMs);
        engine.stop();
    }

    // The same start outliving the ENGINE: the detached bin finishes starting
    // after the engine is destroyed and pushes frames through its encrypt
    // probe and self-view sink, both of which hold raw engine pointers. They
    // are gated now (closed at teardown), so they do nothing. NOT A PROOF
    // without ASan: without the gate this is a use-after-free that usually
    // passes silently; only an ASan build fails reliably on the old code. It
    // does prove stop() does not wait for the start.
    void anEngineDestroyedDuringAPortalCameraStartLeavesItInert()
    {
        static const bool registered =
            gst_element_register(nullptr, "lightningslowstartcam",
                                 GST_RANK_NONE, slow_start_cam_get_type());
        QVERIFY(registered);
        auto *engine = new SfuMediaEngine;
        engine->setTestSourceMode(true);
        engine->setSelfViewInTestModeForTest(true);
        engine->setVideoSourceForTest(QStringLiteral("lightningslowstartcam"));
        engine->start();
        engine->setEncryptionRequired(true);
        engine->setOutboundKey(3, QByteArray(32, 'k'));
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        engine->publishVideo(QStringLiteral("cid-portal-camera"),
                             /*screenShare=*/false, /*nodeId=*/-1, fd);
        // The destructor may also wait (bounded) for a retiring webrtcbin, so
        // only what it shares with stop() is timed: the engine goes while the
        // camera is still starting either way.
        QElapsedTimer took;
        took.start();
        engine->stop();
        const qint64 ms = took.elapsed();
        delete engine;
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("stop() waited %1 ms for a camera "
                                           "still starting")
                                .arg(ms)));
        // The start completes now, frames flow into the gated callbacks.
        QTest::qWait(kSlowStartCamMs + 1500);
    }

    // The offer/caps timing of an asynchronously started camera, end to end
    // (the working-portal case was not testable live on Debian 12): a camera
    // whose start blocks is still offered, negotiated, encrypted and
    // decrypted by the far end. A regression guard for the async start.
    void anAsyncStartedPortalCameraReachesTheFarEndDecrypted()
    {
        static const bool registered =
            gst_element_register(nullptr, "lightningslowstartcam",
                                 GST_RANK_NONE, slow_start_cam_get_type());
        QVERIFY(registered);
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QSignalSpy trackFailed(&sender, &SfuMediaEngine::publishFailed);
        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("cid-portal-camera"), 3, key);
        sender.setVideoSourceForTest(QStringLiteral("lightningslowstartcam"));
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        sender.publishVideo(QStringLiteral("cid-portal-camera"),
                            /*screenShare=*/false, /*nodeId=*/-1, fd);
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 10,
            qPrintable(QStringLiteral("async-started camera: %1 frames "
                                      "decrypted, %2 encrypted; failure=%3")
                           .arg(receiver.framesDecrypted())
                           .arg(sender.framesEncrypted())
                           .arg(failure)),
            45000);
        QCOMPARE(trackFailed.count(), 0);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.stop();
        receiver.stop();
    }

    // The control: a camera that delivers is never reported, and neither is
    // a silent screen share (an on-damage capture legitimately waits for the
    // screen to change), nor a silent camera that was unpublished first.
    void aCameraThatDeliversOrAShareThatWaitsIsNotReported()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setCameraFirstFrameTimeoutForTest(1000);
        QSignalSpy trackFailed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        // Default test source: videotestsrc, which delivers.
        engine.publishVideo(QStringLiteral("cid-camera"),
                            /*screenShare=*/false, -1, -1);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-camera")));
        engine.setVideoSourceForTest(
            QStringLiteral("appsrc is-live=true format=time"));
        engine.publishVideo(QStringLiteral("cid-share"),
                            /*screenShare=*/true, -1, -1);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-share")));
        engine.publishVideo(QStringLiteral("cid-gone"),
                            /*screenShare=*/false, -1, -1);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-gone")));
        engine.unpublish(QStringLiteral("cid-gone"));
        QTest::qWait(4000);
        QString reported;
        if (!trackFailed.isEmpty()) {
            reported = QStringLiteral("%1 %2").arg(
                trackFailed.at(0).at(0).toString(),
                trackFailed.at(0).at(1).toString());
        }
        QVERIFY2(trackFailed.isEmpty(), qPrintable(reported));
        engine.stop();
    }

    // One failed video publish poisoned every later one (found live: a busy
    // camera, -16 or -22 at negotiation, then a screen share published with
    // no frames at all, and the camera again, the same). The failed branch's
    // self-view sink waited for a first frame that never came, so the
    // publisher pipeline sat in PAUSED and every later branch synced to it.
    void aFailedVideoPublishDoesNotStallTheNextOne()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        sender.setSelfViewInTestModeForTest(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QSignalSpy trackFailed(&sender, &SfuMediaEngine::publishFailed);
        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("cid-share"), 3, key);

        // A camera that opens, never delivers a frame and then errors, as a
        // busy device does at negotiation (-16, -22).
        sender.setVideoSourceForTest(
            QStringLiteral("appsrc is-live=true format=time"));
        sender.publishVideo(QStringLiteral("cid-camera"), /*screenShare=*/false,
                            -1, -1);
        {
            GstElement *bin =
                sender.publishedBinForTest(QStringLiteral("cid-camera"));
            QVERIFY(bin);
            GstElement *camera = gst_bin_get_by_name(GST_BIN(bin), "capsrc");
            QVERIFY(camera);
            QTest::qWait(300);
            GError *error = g_error_new_literal(
                GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_BUSY,
                "error set output format: -16 (Device or resource busy)");
            gst_element_post_message(
                camera, gst_message_new_error(GST_OBJECT(camera), error,
                                              "test"));
            g_error_free(error);
            gst_object_unref(camera);
        }
        QTRY_COMPARE_WITH_TIMEOUT(trackFailed.count(), 1, 10000);
        QCOMPARE(trackFailed.at(0).at(1).toString(),
                 QStringLiteral("camera_failed"));
        // The next video publish must reach the far end, even while the
        // failed branch is still in the pipeline (the controller removes it
        // on the report, but nothing may depend on that having finished).
        sender.setVideoSourceForTest(QString());
        sender.publishVideo(QStringLiteral("cid-share"), /*screenShare=*/true,
                            -1, -1);
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 10,
            qPrintable(QStringLiteral("the share after a failed camera: %1 "
                                      "frames decrypted, %2 encrypted; "
                                      "failure=%3")
                           .arg(receiver.framesDecrypted())
                           .arg(sender.framesEncrypted())
                           .arg(failure)),
            45000);
        // And removing the failed branch later changes nothing.
        sender.unpublish(QStringLiteral("cid-camera"));
        QTRY_VERIFY_WITH_TIMEOUT(
            !sender.hasPublishedBinForTest(QStringLiteral("cid-camera")), 5000);
        const quint64 before = receiver.framesDecrypted();
        QTRY_VERIFY_WITH_TIMEOUT(receiver.framesDecrypted() > before + 10,
                                 10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.stop();
        receiver.stop();
    }

    // A remote stream's jitterbuffer and its two threads must go when the
    // stream does: rtpbin keeps a timed-out source for good by default, so
    // every remote rejoin grew the process by two threads (measured live,
    // 85 -> 128 over 20 rejoins; threadgrow.c: +2 per new SSRC, reclaimed
    // only with autoremove). The wait for a source timeout (20-40 s) does
    // not fit this suite's budget, so the setting itself is asserted.
    // A muted track times out after ~40 s. With autoremove its source went,
    // and the unmuted SSRC's new pad was linked to nothing: the flow error
    // stopped the shared transport and every received track went silent.
    void theSubscriberNeverDropsARemoteSourceOnTimeout()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        QCOMPARE(engine.subscriberAutoremoveForTest(), -1);
        engine.applyRemoteDescription(
            SfuMediaEngine::Target::Subscriber, QStringLiteral("offer"),
            QStringLiteral("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
                           "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                           "c=IN IP4 0.0.0.0\r\na=mid:0\r\n"
                           "a=ice-ufrag:abcd\r\na=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
                           "a=fingerprint:sha-256 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00\r\n"
                           "a=setup:actpass\r\na=sctp-port:5000\r\n"));
        QCOMPARE(engine.subscriberAutoremoveForTest(), 0);
        engine.stop();
    }

    // A microphone whose capture fails mid-call (measured: pulsesrc "Failed
    // to connect stream: Timeout" while the sound server stalled) stayed dead
    // for the whole call: nothing retried it, and the track stayed declared
    // and silent. It is restarted in place, keeping its published track.
    void aMicrophoneCaptureThatFailsIsRestartedAndKeepsItsTrack()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        sender.setCaptureRestartDelayForTest(50);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        QSignalSpy publishFailures(&sender, &SfuMediaEngine::publishFailed);

        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("first"), 3, key);
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrived.isEmpty(),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        QTRY_VERIFY_WITH_TIMEOUT(receiver.framesDecrypted() > 20, 15000);
        const int trackSlots = sender.publisherTrackSlotsForTest();

        // The source's next buffer fails, the way a lost sound server does;
        // basesrc then posts the error and stops its task.
        GstElement *bin = sender.publishedBinForTest(QStringLiteral("first"));
        QVERIFY(bin);
        GstElement *source = gst_bin_get_by_name(GST_BIN(bin), "micsrc");
        QVERIFY(source);
        GstPad *out = gst_element_get_static_pad(source, "src");
        static std::atomic<int> failNext{0};
        failNext.store(1);
        gst_pad_add_probe(
            out, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *info, gpointer) {
                if (failNext.fetch_sub(1) <= 0) {
                    failNext.store(0);
                    return GST_PAD_PROBE_OK;
                }
                gst_buffer_unref(GST_PAD_PROBE_INFO_BUFFER(info));
                GST_PAD_PROBE_INFO_DATA(info) = nullptr;
                GST_PAD_PROBE_INFO_FLOW_RETURN(info) = GST_FLOW_ERROR;
                return GST_PAD_PROBE_HANDLED;
            },
            nullptr, nullptr);
        gst_object_unref(out);
        gst_object_unref(source);

        QTRY_VERIFY_WITH_TIMEOUT(sender.microphoneRestartsForTest() >= 1,
                                 5000);
        // Heard again, on the same track.
        const quint64 before = receiver.framesDecrypted();
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > before + 50,
            qPrintable(QStringLiteral("after the capture failed the far end "
                                      "decrypted %1 more frames")
                           .arg(receiver.framesDecrypted() - before)),
            10000);
        QVERIFY(sender.hasPublishedBinForTest(QStringLiteral("first")));
        QCOMPARE(sender.publisherTrackSlotsForTest(), trackSlots);
        QCOMPARE(publishFailures.count(), 0);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // A microphone that never opens (a real refusal: pulsesrc pointed at a
    // socket nobody listens on) is retried a bounded number of times and then
    // reported as one track failure; the call itself goes on.
    void aMicrophoneThatNeverOpensIsReportedWithoutEndingTheCall()
    {
        GstElementFactory *factory = gst_element_factory_find("pulsesrc");
        if (!factory)
            QSKIP("no pulsesrc: this refusal cannot be staged here");
        gst_object_unref(factory);

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneSourceForTest(QStringLiteral(
            "pulsesrc server=unix:/nonexistent/lightning-test-no-server"));
        engine.setCaptureRestartDelayForTest(20);
        QSignalSpy fatal(&engine, &SfuMediaEngine::failed);
        QSignalSpy trackFailed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-mic"));

        // 20+40+80+160+320 ms of restarts, then one report.
        QTRY_COMPARE_WITH_TIMEOUT(trackFailed.count(), 1, 10000);
        QCOMPARE(trackFailed.at(0).at(0).toString(),
                 QStringLiteral("cid-mic"));
        QCOMPARE(trackFailed.at(0).at(1).toString(),
                 QStringLiteral("audio_source_failed"));
        QCOMPARE(engine.microphoneRestartsForTest(), 5);
        QTest::qWait(500);
        QCOMPARE(trackFailed.count(), 1);
        QCOMPARE(fatal.count(), 0);
        engine.stop();
    }

    // LOW (Flatpak RC): once the restart budget was spent the microphone was
    // dead for the rest of the call. It is tried again on a slow cadence, and
    // when it delivers again the controller is told, so the notice goes.
    void aMicrophoneGivenUpOnIsRetriedAndReportedBack()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setCaptureRestartDelayForTest(20);
        engine.setMicSlowRetryForTest(300);
        QSignalSpy trackFailed(&engine, &SfuMediaEngine::publishFailed);
        QSignalSpy recovered(&engine, &SfuMediaEngine::microphoneRecovered);
        engine.start();
        const QString cid = QStringLiteral("cid-mic");
        engine.publishAudio(cid);
        QTest::qWait(200);
        // Five failures in quick succession spend the budget; the sixth
        // gives up and reports.
        for (int i = 0; i < 6 && trackFailed.isEmpty(); ++i) {
            engine.handleCaptureError(cid);
            QTest::qWait(1500);
        }
        QCOMPARE(trackFailed.count(), 1);
        QCOMPARE(engine.microphoneRestartsForTest(), 5);
        // A retry samples the device for a second. This fixture has no
        // transport, so its source stops being pulled after a few buffers:
        // not delivering, so it is retried, not reported back.
        QTest::qWait(1800);
        QCOMPARE(recovered.count(), 0);
        // Now it delivers, continuously (simulated): reported back.
        QTimer feed;
        QObject::connect(&feed, &QTimer::timeout, &engine,
                         [&engine] { engine.addMicCapturedForTest(5); });
        feed.start(50);
        QTRY_COMPARE_WITH_TIMEOUT(recovered.count(), 1, 5000);
        feed.stop();
        QCOMPARE(recovered.at(0).at(0).toString(), cid);
        QCOMPARE(engine.microphoneRestartsForTest(), 0);
        // And a later failure is handled normally again.
        engine.handleCaptureError(cid);
        QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneRestartsForTest(), 1, 3000);
        QCOMPARE(trackFailed.count(), 1);
        engine.stop();
    }

    // A microphone picked during a group call reaches the far end at once,
    // on the same track: until 2026-10-09 setPreferredDevices() only stored
    // the choice, and the call kept sending the old device until the next
    // publish. Silent fake microphone first, a loud one picked mid-call, and
    // back; the receiver's decoded output is what is measured.
    void aMicrophonePickedMidCallReachesTheFarEnd()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        sender.setNoiseSuppressionMode(calls::noise::Mode::Off);
        const QString silent =
            QStringLiteral("audiotestsrc is-live=true wave=silence");
        const QString loud = QStringLiteral(
            "audiotestsrc is-live=true wave=sine freq=440 volume=0.5");
        sender.setMicrophoneSourceForTest(silent);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        int offers = 0;
        connect(&sender, &SfuMediaEngine::localDescription, this,
                [&](int target, const QString &kind, const QString &) {
                    if (target == int(SfuMediaEngine::Target::Publisher)
                        && kind == QStringLiteral("offer"))
                        ++offers;
                });
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });

        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("first"), 3, key);
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrived.isEmpty(),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 0,
            qPrintable(QStringLiteral("nothing decrypted; failure=%1")
                           .arg(failure)),
            45000);

        // The far end's decoded audio, at the track's volume element.
        auto meter = std::make_shared<PeakMeter>();
        GstElement *bin = receiver.receiveBinForTest(QStringLiteral("first"));
        QVERIFY(bin);
        GstElement *volume = elementWithPrefix(bin, "outvol_");
        QVERIFY(volume);
        GstPad *heard = gst_element_get_static_pad(volume, "src");
        gst_object_unref(volume);
        QVERIFY(heard);
        meterPeaksAt(heard, meter);
        gst_object_unref(heard);
        const auto peakOver = [&meter](int ms) {
            meter->peakMilli.store(0);
            const int buffers = meter->buffers.load();
            QTest::qWait(ms);
            return meter->buffers.load() > buffers ? meter->peakMilli.load()
                                                   : -1;
        };

        GstElement *sendBin =
            sender.publishedBinForTest(QStringLiteral("first"));
        QVERIFY(sendBin);
        GstElement *valve = gst_bin_get_by_name(GST_BIN(sendBin), "micvalve");
        QVERIFY(valve);
        const QString encoderCaps = padCapsOf(sendBin, "audioenc", "sink");
        QVERIFY(!encoderCaps.isEmpty());

        QTest::qWait(500);
        const int before = peakOver(1000);
        QVERIFY2(before >= 0, "no decoded audio reached the receiver");
        QVERIFY2(before < 20, qPrintable(QStringLiteral(
                                   "the silent microphone peaked at %1/1000")
                                   .arg(before)));
        const int offersBefore = offers;
        const quint64 droppedBefore = receiver.framesDropped();

        // The pick, as AppController hands it over on activeDevicesChanged.
        sender.setMicrophoneSourceForTest(loud);
        sender.setPreferredDevices({}, {QStringLiteral("loud-mic"),
                                        QStringLiteral("Loud microphone")},
                                   {});
        int during = -1;
        QTRY_VERIFY2_WITH_TIMEOUT(
            (during = peakOver(250)) > 100,
            qPrintable(QStringLiteral("the far end never heard the microphone "
                                      "picked mid-call: peak %1/1000")
                           .arg(during)),
            10000);

        // The same track: nothing renegotiated, no second track, nothing
        // undecryptable, the encoder's caps and the mute valve unchanged.
        QCOMPARE(offers, offersBefore);
        QCOMPARE(arrived.size(), 1);
        QCOMPARE(receiver.framesDropped(), droppedBefore);
        QCOMPARE(padCapsOf(sendBin, "audioenc", "sink"), encoderCaps);
        GstElement *valveNow =
            gst_bin_get_by_name(GST_BIN(sendBin), "micvalve");
        QCOMPARE(valveNow, valve);
        if (valveNow)
            gst_object_unref(valveNow);
        gst_object_unref(valve);
        // The level meter follows the new device.
        QTRY_VERIFY2_WITH_TIMEOUT(
            sender.micPeakDbForTest() > -30.0
                && sender.micPeakDbForTest() < 0.0,
            qPrintable(QStringLiteral("the meter read %1 dBFS")
                           .arg(sender.micPeakDbForTest())),
            5000);

        // The mute valve still governs the new microphone.
        sender.setMicrophoneMuted(true);
        QTest::qWait(500);
        const int muted = peakOver(1000);
        QVERIFY2(muted < 20, qPrintable(QStringLiteral(
                                 "muted, the far end still heard %1/1000")
                                 .arg(muted)));
        sender.setMicrophoneMuted(false);
        QTRY_VERIFY2_WITH_TIMEOUT(peakOver(250) > 100,
                                  "unmuted, the new microphone is not heard",
                                  10000);

        // And back to the silent one.
        sender.setMicrophoneSourceForTest(silent);
        sender.setPreferredDevices({}, {QStringLiteral("quiet-mic"),
                                        QStringLiteral("Quiet microphone")},
                                   {});
        int after = -1;
        QTRY_VERIFY2_WITH_TIMEOUT(
            (after = peakOver(250)) >= 0 && after < 20,
            qPrintable(QStringLiteral("switched back, the far end still "
                                      "hears %1/1000")
                           .arg(after)),
            10000);
        QCOMPARE(offers, offersBefore);
        QCOMPARE(arrived.size(), 1);
        QCOMPARE(receiver.framesDropped(), droppedBefore);
        QCOMPARE(padCapsOf(sendBin, "audioenc", "sink"), encoderCaps);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // A microphone that will not deliver costs the call nothing: the switch
    // is abandoned and the old microphone keeps being heard.
    void aMicrophoneThatNeverDeliversLeavesTheCallOnTheOldOne()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        sender.setNoiseSuppressionMode(calls::noise::Mode::Off);
        sender.setMicrophoneSwapTimeoutForTest(1500);
        sender.setMicrophoneSourceForTest(QStringLiteral(
            "audiotestsrc is-live=true wave=sine freq=440 volume=0.5"));
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.receiveBinForTest(QStringLiteral("first")) != nullptr,
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        auto meter = std::make_shared<PeakMeter>();
        GstElement *volume = elementWithPrefix(
            receiver.receiveBinForTest(QStringLiteral("first")), "outvol_");
        QVERIFY(volume);
        GstPad *heard = gst_element_get_static_pad(volume, "src");
        gst_object_unref(volume);
        meterPeaksAt(heard, meter);
        gst_object_unref(heard);
        QTRY_VERIFY_WITH_TIMEOUT(meter->peakMilli.load() > 100, 10000);

        // A live source that never produces a buffer.
        sender.setMicrophoneSourceForTest(
            QStringLiteral("appsrc is-live=true format=time "
                           "caps=audio/x-raw,format=S16LE,layout=interleaved,"
                           "rate=48000,channels=1"));
        sender.setPreferredDevices({}, {QStringLiteral("dead-mic"),
                                        QStringLiteral("Dead microphone")},
                                   {});
        QTRY_COMPARE_WITH_TIMEOUT(sender.microphoneSwapsAbandonedForTest(), 1,
                                  5000);
        QCOMPARE(sender.microphoneSwapsForTest(), 0);
        meter->peakMilli.store(0);
        QTRY_VERIFY2_WITH_TIMEOUT(meter->peakMilli.load() > 100,
                                  "the old microphone stopped being heard",
                                  5000);
        sender.stop();
        receiver.stop();
    }

    // A speaker picked during a group call moves every received track to it
    // at once; until 2026-10-09 only tracks arriving later used it.
    void aSpeakerPickedMidCallMovesTheReceivedTracks()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setSpeakerSinkForTest(QStringLiteral("fakesink sync=false"));
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.receiveBinForTest(QStringLiteral("first")) != nullptr,
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        GstElement *bin = receiver.receiveBinForTest(QStringLiteral("first"));
        GstElement *volume = elementWithPrefix(bin, "outvol_");
        QVERIFY(volume);
        QVERIFY(!gst_bin_get_by_name(GST_BIN(bin), "outsink"));

        receiver.setPreferredDevices({}, {}, {QStringLiteral("other-output"),
                                              QStringLiteral("Other output")});
        QTRY_COMPARE_WITH_TIMEOUT(receiver.speakerSwapsForTest(), 1, 5000);
        GstElement *outsink = gst_bin_get_by_name(GST_BIN(bin), "outsink");
        QVERIFY(outsink);
        static std::atomic<int> reached{0};
        reached.store(0);
        countBuffersAt(outsink, &reached);
        gst_object_unref(outsink);
        QTRY_VERIFY2_WITH_TIMEOUT(reached.load() > 10,
                                  "nothing reached the new output", 10000);
        // The same chain: its volume element (level, deafen) stays, and one
        // output is left.
        QCOMPARE(receiver.receiveBinForTest(QStringLiteral("first")), bin);
        GstElement *volumeNow = elementWithPrefix(bin, "outvol_");
        QCOMPARE(volumeNow, volume);
        if (volumeNow)
            gst_object_unref(volumeNow);
        gst_object_unref(volume);
        int sinks = 0;
        GstIterator *it = gst_bin_iterate_sinks(GST_BIN(bin));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            ++sinks;
            g_value_reset(&item);
        }
        if (G_IS_VALUE(&item))
            g_value_unset(&item);
        gst_iterator_free(it);
        QCOMPARE(sinks, 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.stop();
        receiver.stop();
    }

    // Lightning's microphone carries the RFC 6464 audio level, which is what
    // LiveKit's speaking indicator reads: without it no client (Element,
    // Sable, Lightning itself) ever showed a Lightning participant speaking.
    // Encrypted, so the frame the payloader sees is the encrypt probe's
    // replacement buffer, which must keep the level meta.
    void theMicrophoneCarriesItsAudioLevelForTheSpeakingIndicator()
    {
        if (!SfuMediaEngine::audioLevelExtensionAvailable())
            QSKIP("no rtphdrextclientaudiolevel or level audio-level-meta");
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QString offer;
        connect(&sender, &SfuMediaEngine::localDescription, this,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Publisher)
                        && kind == QStringLiteral("offer"))
                        offer = sdp;
                });
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });

        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("first"), 3, key);
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrived.isEmpty(),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        QVERIFY2(offer.contains(QStringLiteral(
                     "urn:ietf:params:rtp-hdrext:ssrc-audio-level")),
                 "the offer declares no audio-level extension");

        // What the SFU reads: the one-byte extension on each RTP packet.
        struct Seen {
            std::atomic<int> packets{0};
            std::atomic<int> withLevel{0};
            std::atomic<int> quietest{0};
            std::atomic<int> loudest{127};
        } seen;
        GstElement *bin = receiver.receiveBinForTest(QStringLiteral("first"));
        QVERIFY(bin);
        GstPad *in = gst_element_get_static_pad(bin, "sink");
        QVERIFY(in);
        gst_pad_add_probe(
            in, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *info, gpointer data) {
                auto *s = static_cast<Seen *>(data);
                GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
                if (!gst_rtp_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info),
                                        GST_MAP_READ, &rtp))
                    return GST_PAD_PROBE_OK;
                s->packets.fetch_add(1);
                gpointer ext = nullptr;
                guint size = 0;
                if (gst_rtp_buffer_get_extension_onebyte_header(
                        &rtp, SfuMediaEngine::kAudioLevelExtId, 0, &ext,
                        &size)
                    && size >= 1) {
                    const int level = static_cast<guint8 *>(ext)[0] & 0x7f;
                    s->withLevel.fetch_add(1);
                    if (level > s->quietest.load())
                        s->quietest.store(level);
                    if (level < s->loudest.load())
                        s->loudest.store(level);
                }
                gst_rtp_buffer_unmap(&rtp);
                return GST_PAD_PROBE_OK;
            },
            &seen, nullptr);
        gst_object_unref(in);

        QTRY_VERIFY_WITH_TIMEOUT(seen.packets.load() > 50, 10000);
        QVERIFY2(seen.withLevel.load() * 10 >= seen.packets.load() * 9,
                 qPrintable(QStringLiteral("%1 of %2 packets carried a level")
                                .arg(seen.withLevel.load())
                                .arg(seen.packets.load())));
        // A 0.05 sine is about -29 dBov; 127 would mean silence.
        QVERIFY2(seen.loudest.load() < 60,
                 qPrintable(QStringLiteral("loudest level %1 -dBov")
                                .arg(seen.loudest.load())));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // Where the extension is missing the microphone publishes exactly as
    // before: nothing in the SDP, and the far end still hears it.
    void aMissingAudioLevelExtensionLeavesThePublishAlone()
    {
        struct Restore {
            ~Restore() { SfuMediaEngine::disableAudioLevelExtensionForTest(false); }
        } restore;
        SfuMediaEngine::disableAudioLevelExtensionForTest(true);
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QString offer;
        connect(&sender, &SfuMediaEngine::localDescription, this,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target == int(SfuMediaEngine::Target::Publisher)
                        && kind == QStringLiteral("offer"))
                        offer = sdp;
                });
        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("first"), 3, key);
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY_WITH_TIMEOUT(receiver.framesDecrypted() > 20, 45000);
        QVERIFY(!offer.isEmpty());
        QVERIFY(!offer.contains(QStringLiteral("ssrc-audio-level")));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.stop();
        receiver.stop();
    }

    // One receive sink failing (pulsesink's "Connection terminated" when the
    // sound server drops the client) returned an error upstream that stopped
    // the shared ICE source: measured live, every participant's audio AND
    // video stopped for good while the UI looked normal. It must stay in its
    // own bin, and that bin must come back.
    void aFailedReceiveSinkIsIsolatedAndRebuilt()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(50);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        QSignalSpy lost(&receiver, &SfuMediaEngine::remotePlaybackFailed);

        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        sender.publishAudio(QStringLiteral("second"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.contains(QStringLiteral("first"))
                && arrived.contains(QStringLiteral("second")),
            qPrintable(QStringLiteral("tracks arrived: %1; failure=%2")
                           .arg(arrived.join(QLatin1Char(',')), failure)),
            45000);

        std::atomic<int> second{0};
        std::atomic<int> firstAgain{0};
        GstElement *secondSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("second")));
        GstElement *firstBin =
            receiver.receiveBinForTest(QStringLiteral("first"));
        GstElement *firstSink = firstSinkOf(firstBin);
        QVERIFY(secondSink && firstSink);
        countBuffersAt(secondSink, &second);
        QTRY_VERIFY_WITH_TIMEOUT(second.load() > 10, 10000);

        failLikeADisconnectedSoundServer(firstSink);

        // The other track goes on.
        const int before = second.load();
        QTest::qWait(3000);
        QVERIFY2(second.load() - before > 50,
                 qPrintable(QStringLiteral(
                     "the other track delivered %1 buffers in the 3 s after "
                     "one sink failed: the failure reached the transport")
                                .arg(second.load() - before)));
        // The failed one is rebuilt, with a bin of its own, and plays.
        QTRY_VERIFY_WITH_TIMEOUT(
            receiver.receiveBinForTest(QStringLiteral("first")) != nullptr
                && receiver.receiveBinForTest(QStringLiteral("first"))
                    != firstBin,
            10000);
        GstElement *rebuiltSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
        QVERIFY(rebuiltSink);
        countBuffersAt(rebuiltSink, &firstAgain);
        QTRY_VERIFY_WITH_TIMEOUT(firstAgain.load() > 20, 10000);
        QCOMPARE(receiver.receiveBinsForTest(), 2);
        QCOMPARE(lost.count(), 0);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // A track retired while its output is being rebuilt must stay retired:
    // the rebuild once took the entry out of the map for its whole run, so a
    // retire in that window found nothing and the rebuild then put a live
    // bin back for a stream the transceiver had already replaced (and its
    // outvol_* shadowed the live one's).
    void aTrackRetiredDuringItsRebuildStaysRetired()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(20);
        // The rebuild job holds off long enough for the retire to land in
        // the middle of it.
        receiver.setRebuildJobDelayForTest(1500);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });

        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrived.isEmpty(),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        GstElement *sink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
        QVERIFY(sink);
        failLikeADisconnectedSoundServer(sink);
        // Isolated; its rebuild job is now waiting.
        QTRY_VERIFY_WITH_TIMEOUT(
            receiver.receiveBinForTest(QStringLiteral("first")) == nullptr,
            5000);
        QTest::qWait(300);
        QCOMPARE(receiver.retireAllReceiveBinsForTest(), 1);
        QCOMPARE(receiver.receiveBinsForTest(), 0);

        // The job finishes; nothing comes back.
        QTest::qWait(2500);
        QCOMPARE(receiver.receiveBinsForTest(), 0);
        QVERIFY(receiver.receiveBinForTest(QStringLiteral("first")) == nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(receiver.receiveVolumeElementsForTest(), 0,
                                  3000);
        sender.stop();
        receiver.stop();
    }

    // When every rebuild fails (no sound server comes back), the engine says
    // so once, and still keeps the failure away from the other tracks.
    void aSinkThatCannotBeRebuiltIsReportedOnce()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(20);
        receiver.failReceiveRebuildsForTest(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        QSignalSpy lost(&receiver, &SfuMediaEngine::remotePlaybackFailed);

        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        sender.publishAudio(QStringLiteral("second"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.contains(QStringLiteral("first"))
                && arrived.contains(QStringLiteral("second")),
            qPrintable(QStringLiteral("tracks arrived: %1; failure=%2")
                           .arg(arrived.join(QLatin1Char(',')), failure)),
            45000);
        std::atomic<int> second{0};
        GstElement *secondSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("second")));
        GstElement *firstSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
        QVERIFY(secondSink && firstSink);
        countBuffersAt(secondSink, &second);

        failLikeADisconnectedSoundServer(firstSink);

        // 20+40+80+160+320 ms of retries, then one notice.
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 10000);
        QCOMPARE(lost.at(0).at(0).toBool(), true);
        const int before = second.load();
        QTest::qWait(1500);
        QCOMPARE(lost.count(), 1);
        QVERIFY2(second.load() - before > 20,
                 "the track that did not fail stopped too");
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // Measured live on Windows 2026-10-07: the audio endpoint went away
    // (AUDCLNT_E_DEVICE_INVALIDATED), five rebuilds failed in 16 s, the chain
    // gave up, and when the device came back a minute later nothing tried
    // again: silent until leave and rejoin. A changed output list must
    // rebuild a given-up chain, ONCE for a whole burst of changes, and the
    // notice must be withdrawn when it plays again.
    void aGivenUpReceiveChainIsRebuiltWhenAnOutputReturns()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(20);
        receiver.setOutputChangeDebounceForTest(200, 2000);
        receiver.failReceiveRebuildsForTest(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        QSignalSpy lost(&receiver, &SfuMediaEngine::remotePlaybackFailed);

        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.contains(QStringLiteral("first")),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        GstElement *firstSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
        QVERIFY(firstSink);
        failLikeADisconnectedSoundServer(firstSink);

        // The whole budget fails, as with the endpoint gone, and it gives up.
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 10000);
        QCOMPARE(lost.at(0).at(0).toBool(), true);
        const int atGiveUp = receiver.receiveRebuildAttemptsForTest();
        QCOMPARE(atGiveUp, 5);
        QVERIFY(receiver.receiveBinForTest(QStringLiteral("first")) == nullptr);
        // Nothing retries by itself.
        QTest::qWait(800);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp);

        // The output comes back, announced as a burst (a device
        // re-enumerating says "changed" several times).
        receiver.failReceiveRebuildsForTest(false);
        for (int i = 0; i < 4; ++i) {
            receiver.notifyAudioOutputsChanged(true);
            QTest::qWait(50);
        }
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.receiveBinForTest(QStringLiteral("first")) != nullptr,
            "a given-up receive chain was not rebuilt when an audio output "
            "came back",
            10000);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp + 1);
        QCOMPARE(receiver.receiveRearmsForTest(), 1);
        std::atomic<int> again{0};
        GstElement *rebuiltSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
        QVERIFY(rebuiltSink);
        countBuffersAt(rebuiltSink, &again);
        QTRY_VERIFY_WITH_TIMEOUT(again.load() > 20, 10000);
        // The "Call audio stopped" notice is withdrawn.
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 2, 5000);
        QCOMPARE(lost.at(1).at(0).toBool(), false);

        // A later change leaves a playing chain alone.
        receiver.notifyAudioOutputsChanged(true);
        QTest::qWait(600);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp + 1);
        QCOMPARE(receiver.receiveRearmsForTest(), 1);
        QCOMPARE(receiver.receiveBinsForTest(), 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // The bound: an output list saying nothing exists rebuilds nothing, and
    // a burst of changes after which the output still will not open costs
    // exactly ONE attempt and no second notice, then waits for the next
    // burst instead of starting another run of retries.
    void aReturningOutputThatStillFailsCostsOneAttemptPerBurst()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(20);
        receiver.setOutputChangeDebounceForTest(200, 2000);
        receiver.failReceiveRebuildsForTest(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        QSignalSpy lost(&receiver, &SfuMediaEngine::remotePlaybackFailed);

        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.contains(QStringLiteral("first")),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        GstElement *firstSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
        QVERIFY(firstSink);
        failLikeADisconnectedSoundServer(firstSink);
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 10000);
        const int atGiveUp = receiver.receiveRebuildAttemptsForTest();
        QCOMPARE(atGiveUp, 5);

        // The device went: no output exists, so nothing is attempted.
        for (int i = 0; i < 3; ++i) {
            receiver.notifyAudioOutputsChanged(false);
            QTest::qWait(50);
        }
        QTest::qWait(600);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp);
        QCOMPARE(receiver.receiveRearmsForTest(), 0);

        // One appears and still will not open: one attempt for the burst.
        for (int i = 0; i < 4; ++i) {
            receiver.notifyAudioOutputsChanged(true);
            QTest::qWait(50);
        }
        QTRY_COMPARE_WITH_TIMEOUT(receiver.receiveRebuildAttemptsForTest(),
                                  atGiveUp + 1, 5000);
        QTest::qWait(800);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp + 1);
        QCOMPARE(lost.count(), 1);
        QVERIFY(receiver.receiveBinForTest(QStringLiteral("first")) == nullptr);

        // The next burst gets one more, and still no second notice.
        receiver.notifyAudioOutputsChanged(true);
        QTRY_COMPARE_WITH_TIMEOUT(receiver.receiveRebuildAttemptsForTest(),
                                  atGiveUp + 2, 5000);
        QTest::qWait(800);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp + 2);
        QCOMPARE(receiver.receiveRearmsForTest(), 2);
        QCOMPARE(lost.count(), 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // A change that lands while the chain is still inside its own budget
    // (maybe during an attempt that was already failing) buys exactly one
    // attempt more when that budget runs out, then the usual single notice.
    void anOutputChangeDuringTheRetriesBuysOneMoreAttempt()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        // 150+300+600+1200+2400 ms: room for a change in the middle.
        receiver.setReceiveRebuildDelayForTest(150);
        receiver.setOutputChangeDebounceForTest(100, 1000);
        receiver.failReceiveRebuildsForTest(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        QSignalSpy lost(&receiver, &SfuMediaEngine::remotePlaybackFailed);

        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.contains(QStringLiteral("first")),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        GstElement *firstSink =
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
        QVERIFY(firstSink);
        failLikeADisconnectedSoundServer(firstSink);
        QTRY_VERIFY_WITH_TIMEOUT(receiver.receiveRebuildAttemptsForTest() >= 1,
                                 5000);
        receiver.notifyAudioOutputsChanged(true);
        QTest::qWait(300);
        QVERIFY2(receiver.receiveRebuildAttemptsForTest() < 5,
                 "the change landed after the budget ran out; the test's "
                 "timing no longer exercises the in-budget case");
        QCOMPARE(receiver.receiveRearmsForTest(), 0);

        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 15000);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), 6);
        QCOMPARE(receiver.receiveRearmsForTest(), 1);
        QTest::qWait(800);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), 6);
        QCOMPARE(lost.count(), 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    // webrtcbin never removes a receiving src pad (it has no pad-removed for
    // them at all in 1.28), so a track the far end retires kept its receive
    // bin and its audio output for the rest of the call: measured live as one
    // extra PipeWire playback stream per screen share. The answer marking the
    // section inactive is what retires it now.
    void aTrackTheFarEndRetiresReleasesItsReceiveBin()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        QSignalSpy removed(&receiver, &SfuMediaEngine::remoteTrackRemoved);

        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.size() >= 1,
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        QCOMPARE(receiver.receiveBinsForTest(), 1);

        // The sender stops the track: its section goes a=inactive.
        sender.unpublish(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.receiveBinsForTest() == 0,
            qPrintable(QStringLiteral("a retired track still holds %1 "
                                      "receive bin(s); failure=%2")
                           .arg(receiver.receiveBinsForTest())
                           .arg(failure)),
            15000);
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);

        // The next track gets a bin of its own, and only that one plays.
        sender.publishAudio(QStringLiteral("second"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            arrived.size() >= 2,
            qPrintable(QStringLiteral("the second track never arrived; "
                                      "failure=%1").arg(failure)),
            45000);
        QCOMPARE(receiver.receiveBinsForTest(), 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
        // And stop() forgets them with the pipeline.
        QCOMPARE(receiver.receiveBinsForTest(), 0);
    }

    // LiveKit hands a transceiver to the next track with a new SSRC, and
    // webrtcbin answers every new SSRC with a NEW src pad on the same
    // transceiver while the old one stays. The older pad's bin must go.
    void aNewStreamOnATransceiverRetiresTheBinBeforeIt()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        int added = 0;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &, const QString &, const QString &) {
                    ++added;
                });

        sender.start();
        receiver.start();
        // Keyed, so the survivor can be shown to decrypt.
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("first"), 3, key);
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            added >= 1,
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        QCOMPARE(receiver.receiveBinsForTest(), 1);

        // Same transceiver, new SSRC.
        GstElement *bin = sender.publishedBinForTest(QStringLiteral("first"));
        QVERIFY(bin);
        GstElement *pay = nullptr;
        GstElement *filter = nullptr;
        GstIterator *it = gst_bin_iterate_elements(GST_BIN(bin));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            const gchar *factory = GST_OBJECT_NAME(
                gst_element_get_factory(element));
            if (g_strcmp0(factory, "rtpopuspay") == 0)
                pay = element;
            else if (g_strcmp0(factory, "capsfilter") == 0)
                filter = element;
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
        QVERIFY(pay && filter);
        const guint ssrc = 0x5eed1234u;
        GstCaps *caps = nullptr;
        g_object_get(filter, "caps", &caps, nullptr);
        QVERIFY(caps);
        caps = gst_caps_make_writable(caps);
        gst_structure_set(gst_caps_get_structure(caps, 0), "ssrc", G_TYPE_UINT,
                          ssrc, nullptr);
        g_object_set(pay, "ssrc", ssrc, nullptr);
        g_object_set(filter, "caps", caps, nullptr);
        gst_caps_unref(caps);

        QTRY_VERIFY2_WITH_TIMEOUT(
            added >= 2,
            qPrintable(QStringLiteral("the new SSRC never produced a pad; "
                                      "failure=%1").arg(failure)),
            20000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.receiveBinsForTest() == 1,
            qPrintable(QStringLiteral("%1 receive bins for one transceiver")
                           .arg(receiver.receiveBinsForTest())),
            5000);
        // The survivor is the live one.
        const quint64 before = receiver.framesDecrypted();
        QTRY_VERIFY_WITH_TIMEOUT(receiver.framesDecrypted() > before + 10,
                                 10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    /// DEFECT 10: a camera is put behind the MJPG decode whenever jpegdec
    /// exists, and only a description that fails to PARSE fell back to raw.
    /// v4l2src's template lists image/jpeg for every camera, so a raw-only
    /// one parsed, failed to negotiate and captured nothing.
    void capsOfferJpegAnswersFromWhatTheDeviceReported()
    {
        const auto offers = [](const char *text) {
            GstCaps *caps = gst_caps_from_string(text);
            const bool answer = SfuMediaEngine::capsOfferJpeg(caps);
            gst_caps_unref(caps);
            return answer;
        };
        QVERIFY(!offers("video/x-raw, format=(string)YUY2, width=(int)1280"));
        QVERIFY(offers("image/jpeg; video/x-raw, format=(string)YUY2"));
        QVERIFY(offers("image/jpeg, width=(int)1280"));
        // Unknown keeps today's MJPG attempt.
        QVERIFY(offers("ANY"));
        QVERIFY(offers("EMPTY"));
        QVERIFY(SfuMediaEngine::capsOfferJpeg(nullptr));
    }

    /// The same end to end, with a source that shares the property: it
    /// parses behind image/jpeg and is raw-only once opened. The camera must
    /// be built on the raw entry and capture.
    void aRawOnlyCameraIsBuiltOnTheRawEntryAndCaptures()
    {
        if (!SfuMediaEngine::jpegCameraChainAvailable())
            QSKIP("no jpegdec here, so the MJPG chain is never tried");
        static const bool registered = gst_element_register(
            nullptr, "lightningrawonlycam", GST_RANK_NONE,
            raw_only_cam_get_type());
        QVERIFY(registered);
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(QStringLiteral("lightningrawonlycam"));
        engine.start();
        const QString cid = QStringLiteral("cid-raw-only-camera");
        engine.publishVideo(cid, /*screenShare=*/false, /*nodeId=*/-1);
        GstElement *bin = engine.publishedBinForTest(cid);
        QVERIFY2(bin, "the camera publish was refused");
        bool jpegDecoder = false;
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(bin));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            GstElementFactory *factory = gst_element_get_factory(
                GST_ELEMENT(g_value_get_object(&item)));
            if (factory
                && g_strcmp0(GST_OBJECT_NAME(factory), "jpegdec") == 0) {
                jpegDecoder = true;
            }
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
        QVERIFY2(!jpegDecoder, "a raw-only camera was put behind jpegdec");
        // And it captures: frames leave `capsrc`.
        std::atomic<int> frames{0};
        GstElement *capture = gst_bin_get_by_name(GST_BIN(bin), "capsrc");
        QVERIFY(capture);
        GstPad *out = gst_element_get_static_pad(capture, "src");
        const gulong probe = gst_pad_add_probe(
            out, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            },
            &frames, nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(frames.load() > 5, 10000);
        gst_pad_remove_probe(out, probe);
        gst_object_unref(out);
        gst_object_unref(capture);
        engine.stop();
    }

    /// webrtcbin's `stun-server` is parsed as a URI and gstwebrtcnice drops
    /// one with no host ("has no host"), which the ICE form `stun:host:port`
    /// is: every STUN server the SFU named was silently ignored. The TURN
    /// form must keep its credentials percent-encoded and its transport.
    /// Checked the way gstwebrtcnice reads them, with GstUri.
    void iceServerUrisTakeTheFormGstwebrtcniceParses()
    {
        // The defect itself: the ICE form has no host as a URI.
        {
            GstUri *raw =
                gst_uri_from_string_escaped("stun:stun.example.org:3478");
            QVERIFY(!raw || !gst_uri_get_host(raw));
            if (raw)
                gst_uri_unref(raw);
        }
        struct StunCase {
            const char *in;
            const char *out;
            const char *host;
            guint port;
        };
        const StunCase stunCases[] = {
            { "stun:stun.example.org:3478", "stun://stun.example.org:3478",
              "stun.example.org", 3478 },
            { "stun:stun.example.org", "stun://stun.example.org",
              "stun.example.org", GST_URI_NO_PORT },
            { "stun:192.0.2.7:19302", "stun://192.0.2.7:19302", "192.0.2.7",
              19302 },
        };
        for (const StunCase &c : stunCases) {
            const QString out =
                SfuMediaEngine::stunServerUri(QString::fromLatin1(c.in));
            QCOMPARE(out, QString::fromLatin1(c.out));
            GstUri *uri = gst_uri_from_string_escaped(out.toUtf8().constData());
            QVERIFY2(uri, c.out);
            QCOMPARE(QString::fromUtf8(gst_uri_get_scheme(uri)),
                     QStringLiteral("stun"));
            QCOMPARE(QString::fromUtf8(gst_uri_get_host(uri)),
                     QString::fromLatin1(c.host));
            QCOMPARE(gst_uri_get_port(uri), c.port);
            gst_uri_unref(uri);
        }
        QVERIFY(SfuMediaEngine::stunServerUri(QStringLiteral("stun:")).isEmpty());
        QVERIFY(SfuMediaEngine::stunServerUri(
                    QStringLiteral("turn:turn.example.org:3478"))
                    .isEmpty());

        const QString turn = SfuMediaEngine::turnServerUri(
            QStringLiteral("turn:turn.example.org:3478?transport=udp"),
            QStringLiteral("1695:id"), QStringLiteral("p@ss w"));
        QCOMPARE(turn, QStringLiteral("turn://1695%3Aid:p%40ss%20w@"
                                      "turn.example.org:3478?transport=udp"));
        GstUri *uri = gst_uri_from_string_escaped(turn.toUtf8().constData());
        QVERIFY(uri);
        QCOMPARE(QString::fromUtf8(gst_uri_get_scheme(uri)),
                 QStringLiteral("turn"));
        QCOMPARE(QString::fromUtf8(gst_uri_get_host(uri)),
                 QStringLiteral("turn.example.org"));
        QCOMPARE(gst_uri_get_port(uri), 3478u);
        QCOMPARE(QString::fromUtf8(gst_uri_get_query_value(uri, "transport")),
                 QStringLiteral("udp"));
        // gstwebrtcnice splits the userinfo at its first ':' and unescapes.
        const QString userinfo = QString::fromUtf8(gst_uri_get_userinfo(uri));
        const qsizetype colon = userinfo.indexOf(QLatin1Char(':'));
        QVERIFY(colon > 0);
        QCOMPARE(QUrl::fromPercentEncoding(userinfo.left(colon).toUtf8()),
                 QStringLiteral("1695:id"));
        QCOMPARE(QUrl::fromPercentEncoding(userinfo.mid(colon + 1).toUtf8()),
                 QStringLiteral("p@ss w"));
        gst_uri_unref(uri);

        const QString turns = SfuMediaEngine::turnServerUri(
            QStringLiteral("turns:turn.example.org:443?transport=tcp"),
            QStringLiteral("u"), QStringLiteral("p"));
        QCOMPARE(turns, QStringLiteral(
                            "turns://u:p@turn.example.org:443?transport=tcp"));
        QVERIFY(SfuMediaEngine::turnServerUri(QStringLiteral("stun:x:1"),
                                              QStringLiteral("u"),
                                              QStringLiteral("p"))
                    .isEmpty());
    }

    /// The same, end to end: a STUN server the SFU names must actually be
    /// asked. The server is a silent socket on this machine's own address,
    /// which sees the Binding Request only if the URI reached libnice.
    void aStunServerFromTheSfuReachesLibnice()
    {
        const QString host = hostIpv4Address();
        if (host.isEmpty())
            QSKIP("no IPv4 interface for libnice to send a Binding Request from");
        SilentUdpPort stun(host);
        if (stun.port() == 0)
            QSKIP("no UDP socket on this machine's address for the STUN server");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        // Not under test here: the silent server would hold the stop 2 s.
        engine.setRetireBoundForTest(500);
        engine.setIceServers(stun.asStunServer());
        engine.start();
        engine.publishAudio(QStringLiteral("cid-stun"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            stun.sawStunBindingRequest(),
            "no STUN Binding Request reached the server the SFU named: the "
            "URI never reached libnice",
            10000);
        engine.stop();
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0, 3000);
    }

    /// GitHub #3 on the MatrixRTC engine: a webrtcbin set to NULL while its
    /// ICE gathering can still emit a candidate aborts the process on every
    /// GStreamer we ship (gstreamer#5138). A TURN server that never answers
    /// holds gathering open; the call stops on the publisher's first
    /// candidate, when gathering has certainly started. The webrtcbin must
    /// reach NULL only after the gathering has ended, and within the bound.
    void stoppingWhileThePublisherGathersStopsWebrtcbinOnlyAfterGathering()
    {
        SilentUdpPort turn;
        if (turn.port() == 0)
            QSKIP("no loopback UDP socket for the silent TURN server");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setIceServers(turn.asTurnServer());
        bool stopped = false;
        connect(&engine, &SfuMediaEngine::localCandidate, this,
                [&](int target, const QString &) {
                    if (target != int(SfuMediaEngine::Target::Publisher)
                        || stopped) {
                        return;
                    }
                    stopped = true;
                    engine.stop();
                });
        engine.start();
        engine.publishAudio(QStringLiteral("cid-gathering"));
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0,
                                  kRetiredWithinMs);
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 0);
        QVERIFY2(engine.retiresThatWaitedOnGatheringForTest() == 1,
                 "gathering had already ended when the call stopped, so this "
                 "run proved nothing: the silent TURN server did not hold it "
                 "open (no IPv4 host candidate to allocate from?)");
        QCOMPARE(engine.retireBoundExpiriesForTest(), 0);
    }

    /// Only webrtcbin waits. The receive bins, their decrypt probes (which
    /// point into the engine) and every sink stop inside stop(), so nothing
    /// is heard or counted after leaving while the webrtcbin finishes
    /// gathering.
    void aStoppedPeersMediaStopsAtOnceWhileWebrtcbinWaits()
    {
        SilentUdpPort turn;
        if (turn.port() == 0)
            QSKIP("no loopback UDP socket for the silent TURN server");
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        // Only the receiver gathers against the silent server, so its
        // subscriber is still gathering when it leaves.
        receiver.setIceServers(turn.asTurnServer());
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QString arrivedStream;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &, const QString &) {
                    arrivedStream = streamId;
                });
        sender.start();
        receiver.start();
        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("sender-device"), 3, key);
        sender.publishAudio(QStringLiteral("cid-stops-at-once"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrivedStream.isEmpty(),
            qPrintable(QStringLiteral("no media pad; failure=%1").arg(failure)),
            45000);
        receiver.noteParticipantIdentity(arrivedStream,
                                         QStringLiteral("sender-device"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 5,
            qPrintable(QStringLiteral("nothing decrypted; failure=%1")
                           .arg(failure)),
            30000);

        receiver.stop();
        const quint64 atStop = receiver.framesDecrypted();
        QTest::qWait(400);
        const bool webrtcbinStillWaiting =
            receiver.retiringWebrtcForTest() >= 1;
        QCOMPARE(receiver.framesDecrypted(), atStop);
        if (!webrtcbinStillWaiting) {
            qWarning("the receiver's gathering had ended before the check, so "
                     "the stop was not observed alongside a waiting webrtcbin "
                     "in this run");
        }
        sender.stop();
        QTRY_COMPARE_WITH_TIMEOUT(receiver.retiringWebrtcForTest(), 0,
                                  kRetiredWithinMs);
        QTRY_COMPARE_WITH_TIMEOUT(sender.retiringWebrtcForTest(), 0,
                                  kRetiredWithinMs);
        QCOMPARE(receiver.teardownsWhileGatheringForTest(), 0);
        QCOMPARE(sender.teardownsWhileGatheringForTest(), 0);
    }

};

QTEST_MAIN(SfuMediaEngineTest)
#include "SfuMediaEngineTest.moc"
