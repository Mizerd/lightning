#include "calls/SfuMediaEngine.h"
#include "calls/CaptureClock.h"

#include "calls/CallFrameCryptor.h"
#include "calls/RtpH264Payloader.h"
#include "calls/RtpVp8Payloader.h"
#include "calls/ShareAudioSources.h"
#include "calls/SfuVideoRouter.h"
#include "calls/WindowCaptureSrc.h"
#include "calls/noise/DenoiseElement.h"
#include "calls/noise/MicProcessing.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QThread>
#include <QSet>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QtTest/QtTest>

#include <QFile>
#include <QUrl>
#include <QVideoSink>

#include <gst/app/gstappsrc.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtphdrext.h>
#include <gst/rtp/gstrtpbasepayload.h>
#include <gst/video/video-event.h>
#include <gst/video/gstvideometa.h>
#if defined(LIGHTNING_HAVE_GST_ALLOCATORS)
#include <gst/allocators/gstdmabuf.h>
#endif
#if defined(Q_OS_LINUX)
#include <sys/mman.h>
#endif

#include <atomic>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <linux/videodev2.h>
#endif

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {

int framesFromASingleCaptureBuffer(const QString &rateStage, int waitMs)
{
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
    gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
    QTest::qWait(waitMs);
    const int frames = seen->load();

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sinkPad);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return frames;
}

QByteArray engineSource()
{
    QFile file(QStringLiteral(LIGHTNING_SFU_ENGINE_SOURCE));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
#define SOURCE_UNDER_TEST engineSource()

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
        if ((ntohl(in->sin_addr.s_addr) >> 16) == 0xA9FE)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &in->sin_addr, text, sizeof text))
            found = QString::fromLatin1(text);
    }
    ::freeifaddrs(list);
    return found;
}

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

constexpr int kRetiredOnceGatheringEndsWithinMs =
    lightning::webrtc::Retirer::kGatheringBoundMs + 3000;
static_assert(kRetiredOnceGatheringEndsWithinMs
                  < lightning::webrtc::Retirer::kGatheringHardBoundMs,
              "the wait is meant to be shorter than the hard bound");

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

struct SlowStartMic {
    GstBin parent;
};
struct SlowStartMicClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowStartMic, slow_start_mic, GST_TYPE_BIN)

GstStateChangeReturn slowStartMicChangeState(GstElement *element,
                                             GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED)
        g_usleep(gulong(kSlowStartCamMs) * 1000);
    return GST_ELEMENT_CLASS(slow_start_mic_parent_class)
        ->change_state(element, transition);
}

void slow_start_mic_init(SlowStartMic *self)
{
    GstElement *device =
        gst_element_factory_make("audiotestsrc", "slowmicdevice");
    g_object_set(device, "is-live", TRUE, "volume", 0.05, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    GstPad *ghost = gst_ghost_pad_new("src", inner);
    gst_object_unref(inner);
    gst_element_add_pad(GST_ELEMENT(self), ghost);
}

void slow_start_mic_class_init(SlowStartMicClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-start microphone (test)",
        "Source/Audio", "A capture whose sound server answers late",
        "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowStartMicChangeState;
}

bool registerSlowStartMic()
{
    static const bool registered =
        gst_element_register(nullptr, "lightningslowstartmic", GST_RANK_NONE,
                             slow_start_mic_get_type());
    return registered;
}

struct SlowStopMic {
    GstBin parent;
};
struct SlowStopMicClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowStopMic, slow_stop_mic, GST_TYPE_BIN)

constexpr int kSlowStopMicMs = 1000;

GstStateChangeReturn slowStopMicChangeState(GstElement *element,
                                            GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED)
        g_usleep(gulong(kSlowStopMicMs) * 1000);
    return GST_ELEMENT_CLASS(slow_stop_mic_parent_class)
        ->change_state(element, transition);
}

void slow_stop_mic_init(SlowStopMic *self)
{
    GstElement *device =
        gst_element_factory_make("audiotestsrc", "slowstopmicdevice");
    g_object_set(device, "is-live", TRUE, "volume", 0.05, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", inner));
    gst_object_unref(inner);
}

void slow_stop_mic_class_init(SlowStopMicClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-stop microphone (test)",
        "Source/Audio", "A capture whose sound server stalls its stop",
        "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowStopMicChangeState;
}

struct StuckStartMic {
    GstBin parent;
};
struct StuckStartMicClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(StuckStartMic, stuck_start_mic, GST_TYPE_BIN)

constexpr int kStuckStartMicMs = 10000;

struct StuckStartGate {
    GMutex mutex;
    GCond cond;
    bool released = false;
    int inFlight = 0;
    StuckStartGate()
    {
        g_mutex_init(&mutex);
        g_cond_init(&cond);
    }
};

StuckStartGate &stuckStartGate()
{
    static StuckStartGate *gate = new StuckStartGate;
    return *gate;
}

GstStateChangeReturn stuckStartMicChangeState(GstElement *element,
                                              GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
        StuckStartGate &gate = stuckStartGate();
        g_mutex_lock(&gate.mutex);
        ++gate.inFlight;
        const gint64 deadline = g_get_monotonic_time()
            + gint64(kStuckStartMicMs) * G_TIME_SPAN_MILLISECOND;
        while (!gate.released
               && g_cond_wait_until(&gate.cond, &gate.mutex, deadline)) {
        }
        g_mutex_unlock(&gate.mutex);
    }
    const GstStateChangeReturn result =
        GST_ELEMENT_CLASS(stuck_start_mic_parent_class)
            ->change_state(element, transition);
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
        StuckStartGate &gate = stuckStartGate();
        g_mutex_lock(&gate.mutex);
        --gate.inFlight;
        g_cond_broadcast(&gate.cond);
        g_mutex_unlock(&gate.mutex);
    }
    return result;
}

struct StuckStartRelease {
    StuckStartRelease()
    {
        StuckStartGate &gate = stuckStartGate();
        g_mutex_lock(&gate.mutex);
        gate.released = false;
        g_mutex_unlock(&gate.mutex);
    }
    ~StuckStartRelease()
    {
        StuckStartGate &gate = stuckStartGate();
        g_mutex_lock(&gate.mutex);
        gate.released = true;
        g_cond_broadcast(&gate.cond);
        const gint64 deadline = g_get_monotonic_time()
            + gint64(kStuckStartMicMs) * G_TIME_SPAN_MILLISECOND;
        while (gate.inFlight > 0
               && g_cond_wait_until(&gate.cond, &gate.mutex, deadline)) {
        }
        g_mutex_unlock(&gate.mutex);
        QTest::qWait(300);
    }
};

void stuck_start_mic_init(StuckStartMic *self)
{
    GstElement *device =
        gst_element_factory_make("audiotestsrc", "stuckmicdevice");
    g_object_set(device, "is-live", TRUE, "volume", 0.05, nullptr);
    gst_bin_add(GST_BIN(self), device);
    GstPad *inner = gst_element_get_static_pad(device, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", inner));
    gst_object_unref(inner);
}

void stuck_start_mic_class_init(StuckStartMicClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Stuck-start microphone (test)",
        "Source/Audio", "A capture whose sound server never answers",
        "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = stuckStartMicChangeState;
}

bool registerStuckStartMic()
{
    static const bool registered =
        gst_element_register(nullptr, "lightningstuckstartmic", GST_RANK_NONE,
                             stuck_start_mic_get_type());
    return registered;
}

bool registerSlowStopMic()
{
    static const bool registered =
        gst_element_register(nullptr, "lightningslowstopmic", GST_RANK_NONE,
                             slow_stop_mic_get_type());
    return registered;
}

struct LiveDisposeCounter {
    static std::atomic<int> &count()
    {
        static std::atomic<int> n{0};
        return n;
    }
    static void handler(const gchar *domain, GLogLevelFlags level,
                        const gchar *message, gpointer previous)
    {
        static const char kPrefix[] = "Trying to dispose element ";
        const char *at = message ? strstr(message, kPrefix) : nullptr;
        if (at) {
            const char *name = at + sizeof kPrefix - 1;
            if (g_str_has_prefix(name, "mic")
                || g_str_has_prefix(name, "slowmic")
                || g_str_has_prefix(name, "slowstopmic")
                || g_str_has_prefix(name, "stuckmic"))
                count().fetch_add(1);
        }
        g_log_default_handler(domain, level, message, previous);
    }
    guint id = 0;
    LiveDisposeCounter()
    {
        count().store(0);
        id = g_log_set_handler("GStreamer", GLogLevelFlags(G_LOG_LEVEL_CRITICAL
                                                           | G_LOG_LEVEL_WARNING),
                               handler, nullptr);
    }
    ~LiveDisposeCounter() { g_log_remove_handler("GStreamer", id); }
};

struct SlowOpenSink {
    GstBin parent;
};
struct SlowOpenSinkClass {
    GstBinClass parent_class;
};
G_DEFINE_TYPE(SlowOpenSink, slow_open_sink, GST_TYPE_BIN)

GstStateChangeReturn slowOpenSinkChangeState(GstElement *element,
                                             GstStateChange transition)
{
    if (transition == GST_STATE_CHANGE_NULL_TO_READY)
        g_usleep(gulong(kSlowStartCamMs) * 1000);
    return GST_ELEMENT_CLASS(slow_open_sink_parent_class)
        ->change_state(element, transition);
}

void slow_open_sink_init(SlowOpenSink *self)
{
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);
    gst_bin_add(GST_BIN(self), sink);
    GstPad *inner = gst_element_get_static_pad(sink, "sink");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("sink", inner));
    gst_object_unref(inner);
}

void slow_open_sink_class_init(SlowOpenSinkClass *klass)
{
    gst_element_class_set_static_metadata(
        GST_ELEMENT_CLASS(klass), "Slow-open output (test)", "Sink/Audio",
        "An output whose sound server answers late", "Lightning");
    GST_ELEMENT_CLASS(klass)->change_state = slowOpenSinkChangeState;
}

bool registerSlowOpenSink()
{
    static const bool registered =
        gst_element_register(nullptr, "lightningslowopensink", GST_RANK_NONE,
                             slow_open_sink_get_type());
    return registered;
}

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

void postSoundServerError(GstElement *sink)
{
    if (!sink)
        return;
    GError *error = g_error_new_literal(
        GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
        "pa_stream_writable_size() failed: Connection terminated");
    gst_element_post_message(
        sink, gst_message_new_error(GST_OBJECT(sink), error, "test"));
    g_error_free(error);
}

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
}

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

class DecryptProbeRig
{
public:
    DecryptProbeRig(SfuMediaEngine &engine, const QString &streamId,
                    bool video = false, const QString &videoCodec = QString())
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
        engine.installDecryptProbeForTest(srcPad, video, streamId, videoCodec);
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
        gst_app_src_push_buffer(GST_APP_SRC(m_src), buffer);
    }
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
    std::atomic<int> *m_passed = nullptr;
    std::atomic<int> *m_passedView = nullptr;
    GstClockTime m_pts = 0;
    bool m_ok = false;
};

QByteArray opusSilenceFrame()
{
    QByteArray silence(80, '\0');
    silence[0] = char(0xf8);
    silence[1] = char(0xff);
    silence[2] = char(0xfe);
    return silence;
}

QByteArray roomTrailer()
{
    return QByteArray("k3P9dQ2mZ7xW4vB8nT1cY6hJ0fL5sG2aE9rU3oKqXiR");
}

struct ScriptedListing {
    QList<lightning::shareaudio::Stream> live;
    QSet<QString> running;
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

namespace {
static void useStandInHardware(bool on)
{
    SfuMediaEngine::clearShareHardwareEncoderFailedForTest();
    if (on) {
        qputenv("LIGHTNING_SHARE_ENCODER", "hardware");
        qputenv("LIGHTNING_SHARE_GPU", "0");
        SfuMediaEngine::setShareHardwareEncoderForTest(
            SfuMediaEngine::ShareHardwareEncoder{
                {"openh264enc", "bitrate=%1 gop-size=%2",
                 "constrained-baseline", false},
                false});
    } else {
        qunsetenv("LIGHTNING_SHARE_ENCODER");
        qunsetenv("LIGHTNING_SHARE_GPU");
        SfuMediaEngine::setShareHardwareEncoderForTest(std::nullopt);
    }
}

static QString subscriberOffer(const QList<QList<int>> &videos, int version)
{
    QStringList mids{QStringLiteral("0"), QStringLiteral("1")};
    for (int i = 0; i < videos.size(); ++i)
        mids << QString::number(i + 2);
    const QString transport = QStringLiteral(
        "a=ice-ufrag:abcd\r\na=ice-pwd:0123456789012345678901234\r\n"
        "a=setup:actpass\r\n");
    QString sdp = QStringLiteral(
        "v=0\r\no=- 1 %1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE %2\r\na=fingerprint:sha-256 %3\r\n")
        .arg(version).arg(mids.join(QLatin1Char(' ')),
             QStringList(32, QStringLiteral("AB")).join(QLatin1Char(':')));
    sdp += QStringLiteral("m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                          "c=IN IP4 0.0.0.0\r\n") + transport
        + QStringLiteral("a=mid:0\r\na=sctp-port:5000\r\n");
    sdp += QStringLiteral("m=audio 9 UDP/TLS/RTP/SAVPF 111 63\r\n"
                          "c=IN IP4 0.0.0.0\r\n") + transport
        + QStringLiteral("a=mid:1\r\na=sendonly\r\na=rtcp-mux\r\n"
                         "a=rtpmap:111 opus/48000/2\r\n"
                         "a=rtpmap:63 red/48000/2\r\na=fmtp:63 111/111\r\n"
                         "a=ssrc:1000 cname:a\r\n"
                         "a=ssrc:1000 msid:PA_a|TR_a TR_a\r\n"
                         "a=msid:PA_a|TR_a TR_a\r\n");
    for (int i = 0; i < videos.size(); ++i) {
        QStringList payloads;
        QString maps;
        for (const int pt : videos[i]) {
            payloads << QString::number(pt);
            maps += pt == 96 ? QStringLiteral("a=rtpmap:96 VP8/90000\r\n")
                : pt == 98   ? QStringLiteral("a=rtpmap:98 VP9/90000\r\n")
                : pt == 45   ? QStringLiteral("a=rtpmap:45 AV1/90000\r\n")
                : pt == 99   ? QStringLiteral("a=rtpmap:99 NOSUCH/90000\r\n")
                             : QStringLiteral(
                                 "a=rtpmap:125 H264/90000\r\n"
                                 "a=fmtp:125 packetization-mode=1;"
                                 "profile-level-id=42e01f;"
                                 "level-asymmetry-allowed=1\r\n");
        }
        const QString id = QStringLiteral("TR_v%1").arg(i);
        sdp += QStringLiteral("m=video 9 UDP/TLS/RTP/SAVPF %1\r\n"
                              "c=IN IP4 0.0.0.0\r\n")
                   .arg(payloads.join(QLatin1Char(' ')))
            + transport
            + QStringLiteral("a=mid:%1\r\na=sendonly\r\na=rtcp-mux\r\n")
                  .arg(i + 2)
            + maps
            + QStringLiteral("a=ssrc:%1 cname:b\r\n"
                             "a=ssrc:%1 msid:PA_b|%2 %2\r\n"
                             "a=msid:PA_b|%2 %2\r\n")
                  .arg(2000 + i).arg(id);
    }
    return sdp;
}
static QString answerTo(SfuMediaEngine &engine, const QString &offer)
{
    QSignalSpy spy(&engine, &SfuMediaEngine::localDescription);
    engine.applyRemoteDescription(SfuMediaEngine::Target::Subscriber,
                                  QStringLiteral("offer"), offer);
    for (int i = 0; i < 40; ++i) {
        for (const auto &emitted : std::as_const(spy)) {
            if (emitted.at(1).toString() == QLatin1String("answer"))
                return emitted.at(2).toString();
        }
        QTest::qWait(50);
    }
    return {};
}
static QStringList sdpSection(const QString &sdp, int index)
{
    return sdp.split(QStringLiteral("m=")).value(index + 1)
        .split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
}
static QStringList sdpEncodings(const QStringList &section)
{
    QStringList names;
    const auto words = section.value(0).split(QLatin1Char(' '));
    for (const QString &pt : words.mid(3)) {
        for (const QString &line : section) {
            if (line.startsWith(QStringLiteral("a=rtpmap:") + pt + QLatin1Char(' ')))
                names << line.section(QLatin1Char(' '), 1).section(QLatin1Char('/'), 0, 0);
        }
    }
    return names;
}
static QStringList withoutTransport(const QStringList &section)
{
    QStringList kept;
    for (const QString &line : section) {
        if (!line.startsWith(QLatin1String("a=ice-"))
            && !line.startsWith(QLatin1String("a=fingerprint"))
            && !line.startsWith(QLatin1String("a=ssrc"))
            && !line.startsWith(QLatin1String("a=msid")))
            kept << line;
    }
    return kept;
}
}


// A depayloader's src pad and the decoder behind it, driven synchronously.
class HeldVideoRig
{
public:
    explicit HeldVideoRig(SfuMediaEngine &engine, const QString &streamId)
    {
        m_src = gst_pad_new("src", GST_PAD_SRC);
        m_sink = gst_pad_new("sink", GST_PAD_SINK);
        g_object_set_data(G_OBJECT(m_src), "rig", this);
        g_object_set_data(G_OBJECT(m_sink), "rig", this);
        gst_pad_set_event_function(m_src, [](GstPad *pad, GstObject *, GstEvent *event) -> gboolean {
            auto *rig = static_cast<HeldVideoRig *>(g_object_get_data(G_OBJECT(pad), "rig"));
            if (gst_video_event_is_force_key_unit(event))
                ++rig->requests;
            gst_event_unref(event);
            return TRUE;
        });
        gst_pad_set_chain_function(m_sink, [](GstPad *pad, GstObject *, GstBuffer *buffer) {
            ++static_cast<HeldVideoRig *>(g_object_get_data(G_OBJECT(pad), "rig"))->delivered;
            gst_buffer_unref(buffer);
            return GST_FLOW_OK;
        });
        gst_pad_set_event_function(m_sink, [](GstPad *, GstObject *, GstEvent *event) -> gboolean {
            gst_event_unref(event);
            return TRUE;
        });
        gst_pad_set_active(m_sink, TRUE);
        gst_pad_set_active(m_src, TRUE);
        gst_pad_link(m_src, m_sink);
        engine.installDecryptProbeForTest(m_src, true, streamId);
        gst_pad_push_event(m_src, gst_event_new_stream_start("held"));
        GstCaps *caps = gst_caps_from_string("video/x-vp8");
        gst_pad_push_event(m_src, gst_event_new_caps(caps));
        gst_caps_unref(caps);
        GstSegment segment;
        gst_segment_init(&segment, GST_FORMAT_TIME);
        gst_pad_push_event(m_src, gst_event_new_segment(&segment));
    }
    ~HeldVideoRig()
    {
        gst_pad_set_active(m_src, FALSE);
        gst_pad_set_active(m_sink, FALSE);
        gst_object_unref(m_src);
        gst_object_unref(m_sink);
    }
    void push(CallFrameCryptor &sender, bool key)
    {
        QByteArray frame(80, '\x5a');
        frame[0] = key ? '\x10' : '\x11';
        const QByteArray wire = sender.encryptFrame(
            frame, key ? CallFrameCryptor::FrameKind::VideoKey
                       : CallFrameCryptor::FrameKind::VideoDelta, 7, ++m_ts);
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, wire.size(), nullptr);
        gst_buffer_fill(buffer, 0, wire.constData(), wire.size());
        if (!key)
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        gst_pad_push(m_src, buffer);
    }
    int delivered = 0;
    int requests = 0;

private:
    GstPad *m_src = nullptr;
    GstPad *m_sink = nullptr;
    quint32 m_ts = 0;
};

static void keyed(CallFrameCryptor &cryptor, int index, char fill)
{
    cryptor.setKey(index, QByteArray(32, fill));
    cryptor.setCurrentKeyIndex(index);
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

    void anIceRestartIsReadFromTheOffersUfrag()
    {
        const QString offer = QStringLiteral(
            "v=0\r\no=- 1 2 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\n"
            "a=ice-ufrag:pionNEW\r\na=ice-pwd:secretsecretsecretsecret\r\n");
        QCOMPARE(SfuMediaEngine::sdpIceUfrag(offer), QStringLiteral("pionNEW"));
        QVERIFY(SfuMediaEngine::sdpIceUfrag(QStringLiteral("v=0\r\n")).isEmpty());
    }

    void aDeviceBindingIsConvertedToThePropertysType()
    {
        QCOMPARE(SfuMediaEngine::applyDevicePropertyForTest(
                     QStringLiteral("fakesrc"), QStringLiteral("num-buffers"),
                     QStringLiteral("7")),
                 QStringLiteral("7"));
        QVERIFY(SfuMediaEngine::applyDevicePropertyForTest(
                    QStringLiteral("fakesrc"), QStringLiteral("num-buffers"),
                    QStringLiteral("banana"))
                    .isEmpty());
        QVERIFY(SfuMediaEngine::applyDevicePropertyForTest(
                    QStringLiteral("fakesrc"), QStringLiteral("num-buffers"),
                    QStringLiteral("-5"))
                    .isEmpty());
        QCOMPARE(SfuMediaEngine::applyDevicePropertyForTest(
                     QStringLiteral("filesrc"), QStringLiteral("location"),
                     QStringLiteral("/dev/video61")),
                 QStringLiteral("/dev/video61"));
        QVERIFY(SfuMediaEngine::applyDevicePropertyForTest(
                    QStringLiteral("fakesrc"), QStringLiteral("device-path"),
                    QStringLiteral("x"))
                    .isEmpty());
    }

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

    void aSustainedlySilentCaptureIsReported()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        QVERIFY(spy.isValid());

        for (qint64 t = 0; t < SfuMediaEngine::kMicSilenceWindowMs; t += 200)
            engine.handleMicLevelAt(-350.0, t);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());

        engine.handleMicLevelAt(-350.0, SfuMediaEngine::kMicSilenceWindowMs);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);
        QVERIFY(engine.microphoneSilentForTest());
    }

    void aQuietMomentIsNotASilentMicrophone()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        for (qint64 t = 0; t < SfuMediaEngine::kMicSilenceWindowMs * 3;
             t += 200) {
            const bool speaks = (t / 200) % 40 == 0;
            engine.handleMicLevelAt(speaks ? -18.0 : -350.0, t);
        }
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());
    }

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

        engine.setMicrophoneMuted(false);
        engine.handleMicLevelAt(-350.0,
                                SfuMediaEngine::kMicSilenceWindowMs + 5000);
        QVERIFY(!engine.microphoneSilentForTest());
        QCOMPARE(spy.count(), 1);
    }

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

    void aMinuteOfVeryQuietCaptureIsLoggedButNeverRaisesTheBadge()
    {
        SfuMediaEngine engine;
        QSignalSpy spy(&engine, &SfuMediaEngine::localAudioSilent);
        for (qint64 t = 0; t <= SfuMediaEngine::kMicQuietWindowMs; t += 200)
            engine.handleMicLevelAt(-70.0, t);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneSilentForTest());
        QVERIFY(engine.microphoneQuietForTest());
        engine.handleMicLevelAt(-20.0, SfuMediaEngine::kMicQuietWindowMs + 200);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!engine.microphoneQuietForTest());
    }

    void theSilenceCeilingIsAPeakNoSpeechStaysUnder()
    {
        QCOMPARE(SfuMediaEngine::micSilenceSince(-84.0, -1, 1000), qint64(-1));
        QCOMPARE(SfuMediaEngine::micSilenceSince(-86.0, -1, 1000), qint64(1000));
        QCOMPARE(SfuMediaEngine::micSilenceSince(-86.0, 1000, 5000),
                 qint64(1000));
        QCOMPARE(SfuMediaEngine::micSilenceSince(-20.0, 1000, 5000),
                 qint64(-1));
        QCOMPARE(SfuMediaEngine::micSilenceSince(-350.0, -1, 0), qint64(0));
        QVERIFY(SfuMediaEngine::micSilenceReached(
            0, SfuMediaEngine::kMicSilenceWindowMs));
        QVERIFY(!SfuMediaEngine::micSilenceReached(-1, 999999));
        QVERIFY(!SfuMediaEngine::micSilenceReached(
            1000, 1000 + SfuMediaEngine::kMicSilenceWindowMs - 1));
        QVERIFY(SfuMediaEngine::micSilenceReached(
            1000, 1000 + SfuMediaEngine::kMicSilenceWindowMs));
    }

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
        QVERIFY(engine.publishedBinHasElementForTest(
            QStringLiteral("cid-audio"), QStringLiteral("audioenc")));
        engine.stop();
    }

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
        SfuMediaEngine::MicrophoneCapture capture;
        capture.source = QStringLiteral(
            "audiotestsrc is-live=true wave=sine freq=440 volume=0.05 "
            "name=micsrc");
        QVERIFY2(built.startsWith(SfuMediaEngine::microphoneFrontDescription(capture)),
                 qPrintable(built));
        engine.stop();
    }

    void everyUnmutedLevelReachesTheMeter()
    {
        SfuMediaEngine engine;
        QSignalSpy levels(&engine, &SfuMediaEngine::localAudioLevel);
        QVERIFY(levels.isValid());
        engine.handleMeterLevel(-20.0);
        engine.handleMeterLevel(-350.0);
        QCOMPARE(levels.count(), 2);
        QCOMPARE(levels.at(0).at(0).toDouble(), -20.0);
        QCOMPARE(levels.at(1).at(0).toDouble(), -350.0);
        engine.setMicrophoneMuted(true);
        engine.handleMeterLevel(-20.0);
        QCOMPARE(levels.count(), 2);
        engine.setMicrophoneMuted(false);
        engine.handleMicLevelAt(-20.0, 100);
        QCOMPARE(levels.count(), 2);
    }

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

        GstStructure *empty = gst_structure_new_empty("level");
        peak = 1.0;
        QVERIFY(!SfuMediaEngine::readLevelPeak(empty, &peak));
        QCOMPARE(peak, 1.0);
        gst_structure_free(empty);
    }

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
        QVERIFY2(!built.contains(QStringLiteral("mix-matrix")),
                 qPrintable(built.left(200)));
        QVERIFY2(!built.contains(QStringLiteral("channel-mask")),
                 qPrintable(built.left(200)));
        QVERIFY(built.contains(QStringLiteral("audio/x-raw,channels=1")));
        QVERIFY2(built.contains(QStringLiteral("leaky=downstream")),
                 qPrintable(built.left(300)));
        QVERIFY2(built.contains(QStringLiteral("max-size-time=100000000")),
                 qPrintable(built.left(300)));
        engine.stop();
    }

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
        QVERIFY(valve < captured);
        QVERIFY(captured < denoise);
        QVERIFY(denoise < sent);
        engine.stop();
    }

    void aMicrophoneTheSuppressorCleansIsNotADeadOne()
    {
        SfuMediaEngine engine;
        QSignalSpy silent(&engine, &SfuMediaEngine::localAudioSilent);
        QSignalSpy meter(&engine, &SfuMediaEngine::localAudioLevel);
        for (qint64 t = 0; t <= SfuMediaEngine::kMicSilenceWindowMs * 2;
             t += 200) {
            engine.handleMicLevelAt(-29.0, t);
            engine.handleMeterLevel(-350.0);
        }
        QVERIFY(!engine.microphoneSilentForTest());
        QCOMPARE(silent.count(), 0);
        QCOMPARE(meter.count(),
                 int(SfuMediaEngine::kMicSilenceWindowMs * 2 / 200 + 1));
        QCOMPARE(meter.last().at(0).toDouble(), -350.0);

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
                return std::make_unique<Counting>(
                    calls::noise::createSuppressor(m), m);
            });
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
                QTRY_VERIFY_WITH_TIMEOUT(Counting::alive().load() >= 1, 30000);
                if (call % 2 == 1) {
                    engine.setNoiseSuppressionMode(calls::noise::Mode::RNNoise);
                    engine.setNoiseSuppressionMode(mode);
                }
                engine.stop();
            }
            QTRY_COMPARE_WITH_TIMEOUT(engine.retiringWebrtcForTest(), 0, 30000);
            QTRY_COMPARE_WITH_TIMEOUT(calls::noise::liveDenoiseElements(), 0,
                                      30000);
            QTRY_COMPARE_WITH_TIMEOUT(Counting::alive().load(), 0, 30000);
        }
        calls::noise::setSuppressorFactoryForTest({});
        QCOMPARE(calls::noise::liveDenoiseElements(), 0);
        QCOMPARE(Counting::alive().load(), 0);
    }

    void aSwitchFromWebrtcKeepsSuppressingUntilTheNewBackendRuns()
    {
        GstElementFactory *factory = gst_element_factory_find("webrtcdsp");
        if (!factory)
            QSKIP("webrtcdsp is not in this GStreamer");
        gst_object_unref(factory);
        static std::atomic<bool> release{false};
        release.store(false);
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
        engine.handleDenoiseStatus(QStringLiteral("rnnoise"),
                                   QStringLiteral("rnnoise"), true, 960);
        QCOMPARE(engine.lastDspFollowUpForTest(), QString());
        QCOMPARE(dspSuppresses(), QStringLiteral("true"));

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
            QVERIFY(built.indexOf(QStringLiteral("micdenoise"))
                    < built.indexOf(QStringLiteral("micdsp")));
        }
        QVERIFY(built.indexOf(QStringLiteral("micvalve"))
                < built.indexOf(QStringLiteral("micdenoise")));
        QVERIFY(built.indexOf(QStringLiteral("micdenoise"))
                < built.indexOf(QStringLiteral("audioenc")));
        engine.stop();
    }

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
            QCOMPARE(engine.publishedBinPropertyForTest(
                         cid, QStringLiteral("micdsp"),
                         QStringLiteral("noise-suppression")),
                     QStringLiteral("false"));
        }
        engine.stop();

        QVERIFY(engine.noiseSuppressionMode() == calls::noise::Mode::RNNoise);
        engine.start();
        engine.publishAudio(cid);
        QVERIFY(engine.lastAudioDescriptionForTest().contains(
            QStringLiteral("mode=rnnoise")));
        engine.stop();
    }

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
        QVERIFY2(failed.count() == 0,
                 failed.isEmpty()
                     ? "no failure"
                     : qPrintable(failed.first().at(0).toString()));
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-audio")));
        QVERIFY(engine.publishedBinHasElementForTest(
            QStringLiteral("cid-audio"), QStringLiteral("audioenc")));
        engine.stop();
    }

    void theMicrophonePrefersPulseOverPipeWireOnLinux()
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QSKIP("Linux element order");
#else
        const QStringList order = SfuMediaEngine::microphoneElementsForTest();
        QVERIFY2(!order.isEmpty(), "no microphone elements offered at all");
        QCOMPARE(order.first(), QStringLiteral("pulsesrc"));
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
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        for (int i = 0; i < 3; ++i) {
            engine.start();
            QVERIFY(engine.active());
        }
        engine.stop();
        QVERIFY(!engine.active());
    }

    void streamInfoTracksReceivedCryptoAndDecodedVideo()
    {
        SfuMediaEngine sender, receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        sender.start();
        receiver.start();
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        const QByteArray key(32, 's');
        sender.setOutboundKey(0, key);
        sender.publishVideo(QStringLiteral("stats-camera"), false, -1);
        QTRY_COMPARE_WITH_TIMEOUT(receiver.streamInfoStats().size(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(receiver.streamInfoStats().first().toMap()
            .value("dropped").toULongLong() > 0, 15000);
        QCOMPARE(receiver.streamInfoStats().first().toMap()
            .value("decrypted").toULongLong(), 0ULL);
        receiver.setInboundKey(QStringLiteral("stats-camera"), 0, key);
        QTRY_VERIFY_WITH_TIMEOUT(receiver.streamInfoStats().first().toMap()
            .value("width").toInt() > 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(sender.streamInfoStats().first().toMap()
            .value("keyframeRequests").toULongLong() > 0, 15000);
        const auto incoming = receiver.streamInfoStats().first().toMap();
        QVERIFY2(incoming.value("keyframeRequests").toULongLong() > 0,
                 "frames dropped for want of a key did not make the receiver "
                 "ask for a keyframe");
        QVERIFY(incoming.value("keyframes").toULongLong() > 0);
        QCOMPARE(incoming.value("encoder").toString(), QStringLiteral("vp8dec"));
        QVERIFY(sender.streamInfoStats().first().toMap()
                    .value("keyframes").toULongLong() > 0);
        QVERIFY(!incoming.value("outgoing").toBool());
        QVERIFY(incoming.value("encrypted").toBool());
        QVERIFY(incoming.value("decrypted").toULongLong() > 0);
        QVERIFY(incoming.value("packets").toULongLong() > 0);
        const auto outgoing = sender.streamInfoStats().first().toMap();
        QVERIFY(outgoing.value("packets").toULongLong() > 0);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.resetStreamInfoBaselines();
        sender.streamInfoStats();
        QTest::qWait(250);
        const auto rate = sender.streamInfoStats().first().toMap();
        QVERIFY(rate.value("kbps").toDouble() > 0);
        QVERIFY(rate.value("fps").toDouble() > 0);
        sender.stop();
        receiver.stop();
        QVERIFY(receiver.streamInfoStats().isEmpty());
    }

    void streamInfoReadsRealVideoCapsAndClearsRetiredTracks()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QVERIFY(engine.streamInfoStats().isEmpty());
        engine.start();
        engine.publishVideo(QStringLiteral("stats-camera"), false, -1);
        QTRY_COMPARE(engine.streamInfoStats().size(), 1);
        QTRY_VERIFY(engine.streamInfoStats().first().toMap().value("width").toInt() > 0);
        const auto row = engine.streamInfoStats().first().toMap();
        QCOMPARE(row.value("codec").toString(), QStringLiteral("VP8"));
        QCOMPARE(row.value("encoder").toString(), QStringLiteral("vp8enc"));
        QCOMPARE(row.value("source").toString(), QStringLiteral("videotestsrc"));
        QVERIFY(row.value("outgoing").toBool());
        QVERIFY(!row.value("screen").toBool());
        engine.resetStreamInfoBaselines();
        QCOMPARE(engine.streamInfoStats().first().toMap().value("kbps").toDouble(), -1.0);
        engine.unpublish(QStringLiteral("stats-camera"));
        QVERIFY(engine.streamInfoStats().isEmpty());
        engine.stop();
        QVERIFY(engine.streamInfoStats().isEmpty());
    }

    void publishingAudioAndVideoBuildsRealPipelines()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();

        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"),false,
-1);
        for (const QList<QVariant> &args : failed) {
            qWarning() << "engine reported failure:"
                       << args.at(0).toString();
        }
        QCOMPARE(failed.count(), 0);
        engine.stop();
    }

    void anOfferIsOnlyMadeOnceThereIsMediaAndThenCarriesIt()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy offers(&engine, &SfuMediaEngine::localDescription);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);

        engine.start();
        QTest::qWait(400);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(offers.count(), 0);

        engine.publishAudio(QStringLiteral("cid-audio"));
        QTRY_VERIFY_WITH_TIMEOUT(offers.count() > 0, 5000);
        QCOMPARE(failed.count(), 0);

        bool sawPublisherOffer = false;
        for (const QList<QVariant> &args : offers) {
            if (args.at(0).toInt() != 0)
                continue;
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

    void aResumeRenegotiatesAPublisherWaitingOnALostAnswer()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy offers(&engine, &SfuMediaEngine::localDescription);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        const auto publisherOffers = [&offers] {
            int n = 0;
            for (const QList<QVariant> &args : std::as_const(offers))
                n += args.at(0).toInt() == 0 && args.at(1).toString() == QLatin1String("offer");
            return n;
        };
        QTRY_COMPARE_WITH_TIMEOUT(publisherOffers(), 1, 5000);
        engine.renegotiateAfterResume();
        QTRY_COMPARE_WITH_TIMEOUT(publisherOffers(), 2, 5000);
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
        engine.unpublish(QStringLiteral("cid-audio"));
        engine.unpublish(QStringLiteral("never-existed"));
        engine.stop();
    }

    void encryptionArmedBeforeMediaExistsIsSafe()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.setEncryptionRequired(true);
        engine.clearKeys();
        QVERIFY(!engine.encryptionActive());
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"), false, -1);
        QTest::qWait(200);
        QCOMPARE(failed.count(), 0);
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
        QVERIFY(!engine.encryptionActive());
    }

    void aKeyReceivedWhileJoiningSurvivesTheSessionStart()
    {
        const QString ring = QStringLiteral("@c:example.org/DEV");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.clearKeys();
        engine.setInboundKey(ring, 2, QByteArray(16, 'k'));
        engine.start();
        QVERIFY2(engine.recvCryptorFor(ring)->hasKey(2),
                 "start() dropped a key received while joining");
        engine.start();
        QVERIFY(engine.recvCryptorFor(ring)->hasKey(2));
        engine.stop();
        QVERIFY(!engine.recvCryptorFor(ring)->hasKey(2));
    }

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

        const int first = log.count("media key ARRIVED");
        engine.setInboundKey(QStringLiteral("@a:example.org:DEV"), 0,
                             QByteArray(32, 'k'));
        QCOMPARE(log.count("media key ARRIVED"), first);

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
        engine.noteParticipantIdentity(QString(),
                                       QStringLiteral("@a:example.org:DEV"));
        QVERIFY2(log.contains("could NOT be bound to a sending stream"),
                 qPrintable(log.text()));
        log.clear();
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
        engine.setOutboundKey(256, QByteArray(32, 'k'));
        QVERIFY(!engine.encryptionActive());
        engine.stop();
    }

    void screenShareWithoutASourceIsRefusedNotGuessed()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(false);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-screen"),
true,-1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("screen_share_no_source"));
        engine.stop();
    }

    void neitherMediaBackendInitialisesGstreamerItself()
    {
        const QString root = QStringLiteral(SOURCE_DIR "/src/calls/");
        for (const QString &name : { QStringLiteral("SfuMediaEngine.cpp"),
                                     QStringLiteral("GstCallMediaBackend.cpp") }) {
            QFile file(root + name);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(root + name));
            QString source = QString::fromUtf8(file.readAll());
            QVERIFY(!source.isEmpty());
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

    void everyPlatformNamesACaptureSourceItActuallyHas()
    {
        const QString camera = SfuMediaEngine::cameraSource();
        const QString screen = SfuMediaEngine::screenShareSource(0, -1);
        QVERIFY(!camera.isEmpty());
        QVERIFY(!screen.isEmpty());
#if defined(Q_OS_WIN)
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
        QVERIFY(camera.startsWith(QStringLiteral("v4l2src")));
        QVERIFY(screen.startsWith(QStringLiteral("pipewiresrc")));
#endif
#if !defined(Q_OS_LINUX)
        QVERIFY2(!camera.contains(QStringLiteral("v4l2"))
                     && !screen.contains(QStringLiteral("pipewire")),
                 "a Linux-only capture element is named on a platform that "
                 "does not have it, so the pipeline can never be built");
#endif
    }

    void theCameraJpegChainBuildsWhereItsElementsExist()
    {
        const bool available = SfuMediaEngine::jpegCameraChainAvailable();

        GstElementFactory *jpegdec = gst_element_factory_find("jpegdec");
        QCOMPARE(available, jpegdec != nullptr);
        if (jpegdec)
            gst_object_unref(jpegdec);

        if (available) {
            const QString entry = SfuMediaEngine::cameraJpegEntry();
            QVERIFY2(entry.contains(QStringLiteral("image/jpeg")),
                     "the chain must ASK for jpeg, or the camera's MJPG mode "
                     "can never be negotiated");
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

    void aSandboxedCameraCapturesThroughThePortalsPipeWireRemote()
    {
        const QString direct = SfuMediaEngine::cameraSource();
        const QString portal = SfuMediaEngine::cameraSource(11);
        QVERIFY(!direct.isEmpty());
        QVERIFY(!portal.isEmpty());

        QCOMPARE(SfuMediaEngine::cameraSource(-1), direct);

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QCOMPARE(portal, direct);
#else
        QVERIFY(direct.startsWith(QStringLiteral("v4l2src")));
        QVERIFY2(portal.startsWith(QStringLiteral("pipewiresrc")),
                 qPrintable(QStringLiteral("not a PipeWire capture: %1")
                                .arg(portal)));
        QVERIFY2(portal.contains(QStringLiteral("fd=11")),
                 qPrintable(QStringLiteral("the portal's remote descriptor is "
                                           "not passed: %1").arg(portal)));

        QVERIFY2(!portal.contains(QStringLiteral("path=")),
                 qPrintable(QStringLiteral(
                     "a node id from another remote cannot name a node in the "
                     "portal's: %1").arg(portal)));
        QVERIFY2(!portal.contains(QStringLiteral("autoconnect=false")),
                 "autoconnect is what selects the camera on this route; "
                 "turning it off leaves the stream connected to nothing");

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
        QVERIFY2(!portal.contains(QStringLiteral("keepalive-time")),
                 "keepalive-time is back; it was measured to freeze a "
                 "PipeWire capture on its first frame");

        QVERIFY2(portal.contains(QStringLiteral("always-copy=true")),
                 qPrintable(QStringLiteral(
                     "the portal camera hands PipeWire's own buffers "
                     "downstream; a renegotiation frees them under "
                     "videoconvert: %1").arg(portal)));
        QVERIFY(!direct.contains(QStringLiteral("always-copy")));
#endif
    }

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

        const QString entry = SfuMediaEngine::portalCameraEntry();
        QVERIFY2(!entry.contains(QLatin1Char(';')),
                 qPrintable(QStringLiteral("a caps list in front of "
                                           "pipewiresrc: %1").arg(entry)));
        QVERIFY(entry.contains(QStringLiteral("video/x-raw")));
        QVERIFY(entry.contains(QStringLiteral("width=(int)[640,")));

        const QString rate = SfuMediaEngine::cameraRateStage(true);
        QVERIFY(rate.contains(QStringLiteral("max-rate=30")));
        QVERIFY(rate.contains(QStringLiteral("name=vidrate")));
        QVERIFY(rate.contains(QStringLiteral("skip-to-first=true")));

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

    void aPortalCameraIsNotBoundToAHostDeviceId()
    {
        const QByteArray source = SOURCE_UNDER_TEST;
        QVERIFY(!source.isEmpty());
        const int at = source.indexOf("const DeviceChoice camera = cameraChoice()");
        QVERIFY2(at > 0, "the camera device-binding site has moved; this case "
                         "is asserting against nothing");
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
        QVERIFY(withFd.indexOf(QStringLiteral("fd="))
                < withFd.indexOf(QStringLiteral("path=")));

        QVERIFY2(withFd.contains(QStringLiteral("min-buffers=1")),
                 "the fd branch must pin min-buffers, not inherit the "
                 "bundled plugin's version-dependent default");

        const QString withoutFd = SfuMediaEngine::screenShareSource(42, -1);
        QVERIFY2(withoutFd.contains(QStringLiteral("min-buffers=1")),
                 "the no-fd branch must pin min-buffers too");
        QVERIFY(!withoutFd.contains(QStringLiteral("fd=")));
        QVERIFY(withoutFd.contains(QStringLiteral("path=42")));
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
        QVERIFY2(!withFd.contains(QStringLiteral("keepalive-time")),
                 "keepalive-time is back; it was measured to freeze the "
                 "capture on its first frame");
    }

    void arefusedScreenShareClosesTheDescriptorItWasGiven()
    {
        int fds[2] = { -1, -1 };
        QCOMPARE(::pipe(fds), 0);
        SfuMediaEngine engine;
        engine.setTestSourceMode(false);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-screen"),
true,-1, fds[0]);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(::fcntl(fds[0], F_GETFD), -1);
        QCOMPARE(errno, EBADF);
        engine.stop();
        ::close(fds[1]);
    }

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

        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target != int(SfuMediaEngine::Target::Publisher)
                        || kind != QStringLiteral("offer")) {
                        return;
                    }
                    receiver.applyRemoteDescription(
                        SfuMediaEngine::Target::Subscriber, kind, sdp);
                });
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
        receiver.noteParticipantIdentity(arrivedStream,
                                         QStringLiteral("sender-device"));

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

        QTRY_VERIFY2_WITH_TIMEOUT(
            sender.micPeakDbForTest() < 0.0,
            qPrintable(QStringLiteral("no level report reached the engine; "
                                      "encrypted=%1")
                           .arg(sender.framesEncrypted())),
            30000);
        QVERIFY2(sender.micPeakDbForTest()
                     > SfuMediaEngine::kMicSilenceCeilingDb,
                 qPrintable(QStringLiteral("a 0.05 sine measured %1 dBFS")
                                .arg(sender.micPeakDbForTest())));
        QVERIFY(!sender.microphoneSilentForTest());

        QCOMPARE(receiver.receiveBinsForTest(), 1);

        sender.stop();
        receiver.stop();

        QCOMPARE(receiver.pendingTeardownsForTest(), 0);
        QCOMPARE(sender.pendingTeardownsForTest(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(sender.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QTRY_COMPARE_WITH_TIMEOUT(receiver.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(sender.teardownsWhileGatheringForTest(), 0);
        QCOMPARE(receiver.teardownsWhileGatheringForTest(), 0);
    }

    void aMicrophoneAheadOfTheClockStillSendsInRealTime()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
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
        QVERIFY2(received >= 100,
                 qPrintable(QStringLiteral("%1 frames reached the receiver in "
                                           "3 s from a source ahead of the "
                                           "clock (sent %2, held %3)")
                                .arg(received)
                                .arg(sender.framesEncrypted())
                                .arg(sender.micBuffersHeldToClock())));
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

    void aMicrophoneThatKeepsTimeIsNeverHeld()
    {
        using lightning::calls::captureIsAhead;
        using lightning::calls::captureRunningTimeHeld;
        const GstClockTime none = GST_CLOCK_TIME_NONE;
        const GstClockTime ms = GST_MSECOND;
        QVERIFY(!captureIsAhead(-40 * qint64(ms)));
        QVERIFY(!captureIsAhead(0));
        QVERIFY(!captureIsAhead(20 * qint64(ms)));
        QVERIFY(captureIsAhead(21 * qint64(ms)));
        QCOMPARE(captureRunningTimeHeld(5000 * ms, 5000 * ms, 40 * ms, false,
                                        none),
                 5000 * ms);
        QCOMPARE(captureRunningTimeHeld(5300 * ms, 5000 * ms, 40 * ms, true,
                                        none),
                 4960 * ms);
        QCOMPARE(captureRunningTimeHeld(5300 * ms, 5000 * ms, 0, true, none),
                 5000 * ms);
        QCOMPARE(captureRunningTimeHeld(4900 * ms, 5000 * ms, 40 * ms, true,
                                        none),
                 4900 * ms);
        QCOMPARE(captureRunningTimeHeld(5050 * ms, 5000 * ms, 40 * ms, true,
                                        4990 * ms),
                 4990 * ms);
        QCOMPARE(captureRunningTimeHeld(100 * ms, 5010 * ms, 40 * ms, false,
                                        4970 * ms),
                 100 * ms);

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
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
        QSignalSpy blocked(&receiver, &SfuMediaEngine::remoteMediaBlocked);
        QVERIFY(blocked.isValid());

        LogCapture log;
        sender.start();
        receiver.start();

        const QByteArray key(32, 'k');
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        sender.setOutboundKey(3, key);
        receiver.setInboundKey(QStringLiteral("a-different-participant"), 3,
                               key);

        sender.publishAudio(QStringLiteral("cid-unkeyed"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            trackArrived,
            qPrintable(QStringLiteral("no media pad; failure=%1").arg(failure)),
            45000);
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

        sender.setEncryptionRequired(true);
        sender.setOutboundKey(3, QByteArray(32, 'k'));
        QCOMPARE(receiver.framesArrivingEncryptedOnAClearCall(), quint64(0));

        sender.publishAudio(QStringLiteral("cid-clear-but-encrypted"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            trackArrived,
            qPrintable(QStringLiteral("no media pad; failure=%1").arg(failure)),
            45000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.framesDecrypted() > 100,
            qPrintable(QStringLiteral("only %1 frames reached the clear "
                                      "path; sent=%2 failure=%3")
                           .arg(receiver.framesDecrypted())
                           .arg(sender.framesEncrypted())
                           .arg(failure)),
            45000);
        QCOMPARE(receiver.framesDropped(), quint64(0));

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

        QCOMPARE(blocked.count(), 0);

        sender.stop();
        receiver.stop();
    }

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

        receiver.setOutputMuted(true);

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
                    if (offeredStream.isEmpty()) {
                        offeredStream = QStringLiteral("cid-loopback-audio");
                        receiver.setParticipantVolume(offeredStream, 0);
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
        QTRY_COMPARE_WITH_TIMEOUT(receiver.receiveVolumeForTest(arrivedStream),
                                  0.0, 5000);
        log.clear();
        receiver.setParticipantVolume(arrivedStream, 100);
        QVERIFY(receiver.receiveVolumeForTest(arrivedStream) > 0.0);
        QVERIFY2(log.contains("participant volume applied"),
                 qPrintable(QStringLiteral("no landing line; log was:\n%1")
                                .arg(log.text())));
        QVERIFY2(log.contains("elements= 1"),
                 qPrintable(QStringLiteral("the landing named no element "
                                           "count; log was:\n%1")
                                .arg(log.text())));
        QCOMPARE(log.count("participant volume applied"), 1);
        receiver.setParticipantVolume(arrivedStream, 100);
        QCOMPARE(log.count("participant volume applied"), 1);
        receiver.setParticipantVolume(arrivedStream, 60);
        QCOMPARE(log.count("participant volume applied"), 2);
        sender.stop();
        receiver.stop();
        QCOMPARE(receiver.receiveVolumeForTest(arrivedStream), -1.0);
    }

    void encryptedVp8SurvivesOurPayloader()
    {
        lightning::rtp::registerVp8Payloader();
        GstElement *pay = gst_element_factory_make(
            lightning::rtp::vp8PayloaderName(), nullptr);
        QVERIFY2(pay, "the VP8 payloader did not register");
        gst_object_unref(pay);

        CallFrameCryptor sender;
        CallFrameCryptor receiver;
        const QByteArray key(16, 'k');
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
                guint header = 1;
                if (p[0] & 0x80) {
                    header = 2;
                    if (p[1] & 0x80)
                        header += (p[2] & 0x80) ? 2 : 1;
                }
                if ((p[0] & 0x10) != 0) {
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
                QVERIFY2((p[0] & 0x80) != 0, "the X (extended) bit is not set");
                QVERIFY2((p[1] & 0x80) != 0, "the I (picture id) bit is not set");
                QVERIFY2((p[2] & 0x80) != 0, "the picture id is not 15-bit (M)");
                pictureIds.insert(((p[2] & 0x7f) << 8) | p[3]);
                if ((p[0] & 0x10) != 0) {
                    ++starts;
                    frameTimestamps.append(gst_rtp_buffer_get_timestamp(&rtp));
                }
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
        QCOMPARE(starts, markers);
        QVERIFY2(!pictureIds.contains(0),
                 "the picture id sequence started at 0");
        QVERIFY2(pictureIds.size() == starts,
                 qPrintable(QStringLiteral("%1 frames carried %2 distinct "
                                           "picture ids")
                                .arg(starts).arg(pictureIds.size())));
        QVERIFY2(frameTimestamps.size() >= 3,
                 "too few frames to judge the timestamps");
        for (int i = 1; i < frameTimestamps.size(); ++i) {
            QVERIFY2(frameTimestamps.at(i) != frameTimestamps.at(i - 1),
                     qPrintable(QStringLiteral(
                         "frames %1 and %2 share RTP timestamp %3")
                             .arg(i - 1).arg(i).arg(frameTimestamps.at(i))));
        }
    }

    void aCaptureFormatFailureIsToldApartFromAMissingSource()
    {
        const QString internal = QStringLiteral("Internal data stream error.");
        QVERIFY(SfuMediaEngine::isCaptureFormatError(
            GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
            QStringLiteral("stream error: unhandled format"), QString()));
        QVERIFY(SfuMediaEngine::isCaptureFormatError(
            GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
            QStringLiteral("stream error: no more input formats"), QString()));
        QVERIFY(SfuMediaEngine::isCaptureFormatError(
            GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, internal,
            QStringLiteral("streaming stopped, reason not-negotiated (-4)")));
        QVERIFY(SfuMediaEngine::isCaptureFormatError(
            GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
            QStringLiteral("stream error: Format negotiation failed"),
            QString()));
        QVERIFY(SfuMediaEngine::isCaptureFormatError(
            GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION, QString(), QString()));

        QVERIFY(!SfuMediaEngine::isCaptureFormatError(
            GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, internal,
            QStringLiteral("streaming stopped, reason error (-5)")));
        QVERIFY(!SfuMediaEngine::isCaptureFormatError(
            GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, internal, QString()));
        QVERIFY(!SfuMediaEngine::isCaptureFormatError(
            GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED,
            QStringLiteral("stream error: target not found"), QString()));
        QVERIFY(!SfuMediaEngine::isCaptureFormatError(
            GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
            QStringLiteral("unhandled format"), QString()));
        QVERIFY(!SfuMediaEngine::isCaptureFormatError(
            GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND,
            QStringLiteral("target not found"), QString()));
        QVERIFY(!SfuMediaEngine::isCaptureFormatError(
            GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_BUSY,
            QStringLiteral("Device is busy"), QString()));
        QVERIFY(!SfuMediaEngine::isCaptureFormatError(
            GST_CORE_ERROR, GST_CORE_ERROR_STATE_CHANGE, QString(), QString()));
    }

    void aSupersededCaptureAttemptStaysStaleUntilItsSourceHasStopped()
    {
        ShareCaptureRetry retry;
        QCOMPARE(retry.stamp(), 0);
        QVERIFY(!retry.isStale(retry.stamp()));

        const int oldError = retry.stamp();
        QVERIFY(retry.begin());
        // What the old attempt posts between the retry and the restart.
        const int oldEos = retry.stamp();
        const int oldSecondaryError = retry.stamp();
        QCOMPARE(oldEos, 0);
        QVERIFY(retry.isStale(oldError));
        QVERIFY(retry.isStale(oldEos));
        QVERIFY(retry.isStale(oldSecondaryError));
        QVERIFY(!retry.begin());

        retry.publish();
        const int newError = retry.stamp();
        QCOMPARE(newError, 1);
        QVERIFY(!retry.isStale(newError));
        QVERIFY(!retry.isStale(-1));
        QVERIFY(retry.isStale(oldEos));
        QVERIFY(!retry.begin());

    }

    void aShareCaptureRestartIsAbandonedOnceTheBinIsTornDown()
    {
        QVERIFY(ShareCaptureRetry::mayRestart(true, true));
        QVERIFY(!ShareCaptureRetry::mayRestart(false, true));
        QVERIFY(!ShareCaptureRetry::mayRestart(true, false));

        const char *description =
            "appsrc name=capsrc is-live=true format=time "
            "caps=video/x-raw,format=BGRx,width=64,height=64,framerate=30/1 "
            "! capsfilter name=capentry "
            "caps=\"video/x-raw(memory:DMABuf),pixel-aspect-ratio=1/1\" "
            "! fakesink sync=false async=false";
        const auto makeBin = [description](GstElement *pipeline) {
            GError *error = nullptr;
            GstElement *bin = gst_parse_bin_from_description(description,
                                                             FALSE, &error);
            if (error)
                g_error_free(error);
            if (bin)
                gst_bin_add(GST_BIN(pipeline), bin);
            return bin;
        };
        const auto stateOf = [](GstElement *bin, const char *name) {
            GstElement *element = gst_bin_get_by_name(GST_BIN(bin), name);
            GstState state = GST_STATE_VOID_PENDING;
            if (element) {
                gst_element_get_state(element, &state, nullptr, GST_SECOND);
                gst_object_unref(element);
            }
            return state;
        };
        const auto entryCaps = [](GstElement *bin) {
            GstElement *entry = gst_bin_get_by_name(GST_BIN(bin), "capentry");
            GstCaps *caps = nullptr;
            g_object_get(entry, "caps", &caps, nullptr);
            gchar *text = caps ? gst_caps_to_string(caps) : nullptr;
            const QString out = QString::fromUtf8(text ? text : "");
            g_free(text);
            if (caps)
                gst_caps_unref(caps);
            gst_object_unref(entry);
            return out;
        };

        GstElement *pipeline = gst_pipeline_new("restart-test");
        GstElement *live = makeBin(pipeline);
        GstElement *gone = makeBin(pipeline);
        QVERIFY(live && gone);
        gst_object_ref(gone);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QCOMPARE(stateOf(live, "capsrc"), GST_STATE_PLAYING);

        ShareCaptureRetry goneRetry;
        QVERIFY(goneRetry.begin());
        gst_bin_remove(GST_BIN(pipeline), gone);
        gst_element_set_state(gone, GST_STATE_NULL);
        QVERIFY(!SfuMediaEngine::restartShareCapture(gone, goneRetry));
        QCOMPARE(goneRetry.stamp(), 0);
        QCOMPARE(stateOf(gone, "capsrc"), GST_STATE_NULL);
        QVERIFY(entryCaps(gone).contains(QLatin1String("DMABuf")));
        gst_object_unref(gone);

        ShareCaptureRetry retry;
        QVERIFY(retry.begin());
        QVERIFY(SfuMediaEngine::restartShareCapture(live, retry));
        QCOMPARE(retry.stamp(), 1);
        QCOMPARE(stateOf(live, "capsrc"), GST_STATE_PLAYING);
        QVERIFY2(!entryCaps(live).contains(QLatin1String("DMABuf")),
                 qPrintable(entryCaps(live)));

        std::atomic<int> captured{0};
        GstElement *source = gst_bin_get_by_name(GST_BIN(live), "capsrc");
        GstPad *pad = gst_element_get_static_pad(source, "src");
        gst_pad_add_probe(
            pad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            },
            &captured, nullptr);
        gst_object_unref(pad);
        GstBuffer *frame = gst_buffer_new_allocate(nullptr, 64 * 64 * 4,
                                                   nullptr);
        GST_BUFFER_PTS(frame) = 0;
        GstFlowReturn flow = GST_FLOW_ERROR;
        g_signal_emit_by_name(source, "push-buffer", frame, &flow);
        gst_buffer_unref(frame);
        gst_object_unref(source);
        QCOMPARE(flow, GST_FLOW_OK);
        QTRY_VERIFY(captured.load() > 0);

        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }

    void theSystemMemoryEntryTakesWhatTheGpuEntryCouldNot()
    {
        const QString gpu = SfuMediaEngine::captureEntryFilter(true);
        const QString cpu = SfuMediaEngine::captureEntryFilter(false);
        QVERIFY2(gpu.contains(QStringLiteral("name=capentry")), qPrintable(gpu));
        QVERIFY2(!cpu.contains(QStringLiteral("name=")),
                 "the CPU entry is named, so two in one bin cannot parse");
        QCOMPARE(cpu, QStringLiteral("capsfilter caps=\"%1\"")
                          .arg(SfuMediaEngine::systemMemoryEntryCaps()));

        GstCaps *fallback = gst_caps_from_string(
            SfuMediaEngine::systemMemoryEntryCaps().toUtf8().constData());
        QVERIFY(fallback);
        for (const char *offer :
             { "video/x-raw,format=BGRx,pixel-aspect-ratio=1/1",
               "video/x-raw,format=BGRA,pixel-aspect-ratio=1/1",
               "video/x-raw,format=RGBx,pixel-aspect-ratio=1/1",
               "video/x-raw,format=NV12,pixel-aspect-ratio=1/1" }) {
            GstCaps *caps = gst_caps_from_string(offer);
            QVERIFY2(gst_caps_can_intersect(fallback, caps), offer);
            gst_caps_unref(caps);
        }
        GstCaps *dmabuf = gst_caps_from_string(
            "video/x-raw(memory:DMABuf),format=DMA_DRM,"
            "drm-format=XR24:0x0300000000606010");
        QVERIFY2(!gst_caps_can_intersect(fallback, dmabuf),
                 "the system-memory fallback admits a DMA-BUF");
        gst_caps_unref(dmabuf);
        gst_caps_unref(fallback);
    }

    void aGpuShareThatCannotNegotiateRetriesInSystemMemoryOnce()
    {
        if (!SfuMediaEngine::missingGpuShareElement().isEmpty()
            || !SfuMediaEngine::gpuShareChainUsable())
            QSKIP("no usable GL chain, so the share never takes the GPU entry");
        SfuMediaEngine::clearShareDmaBufCaptureFailedForTest();
        SfuMediaEngine::setPipewirePluginVersionForTest(QStringLiteral("1.6.6"));
        const auto restore = qScopeGuard([] {
            SfuMediaEngine::clearShareDmaBufCaptureFailedForTest();
            SfuMediaEngine::setPipewirePluginVersionForTest(QString());
        });
        const QString cid = QStringLiteral("cid-share");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(QStringLiteral(
            "appsrc is-live=true format=time "
            "caps=video/x-raw,format=BGRx,width=64,height=64,"
            "framerate=30/1"));
        QSignalSpy failed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        engine.publishVideo(cid, true, 0);
        QVERIFY(engine.hasPublishedBinForTest(cid));
        QVERIFY2(engine.shareEncoderInUse().contains(QLatin1String("gpu")),
                 qPrintable(engine.shareEncoderInUse()));
        QVERIFY(engine.publishedBinPropertyForTest(
                          cid, QStringLiteral("capentry"),
                          QStringLiteral("caps"))
                    .contains(QLatin1String("DMABuf")));

        engine.handlePublishError(cid, false, false, true, 0);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(engine.shareCaptureEpochForTest(cid), 1);
        QTRY_VERIFY(!engine.publishedBinPropertyForTest(
                               cid, QStringLiteral("capentry"),
                               QStringLiteral("caps"))
                         .contains(QLatin1String("DMABuf")));
        QVERIFY2(SfuMediaEngine::shareDmaBufCaptureFailedForTest(),
                 "a failed DMA-BUF negotiation was not remembered");

        engine.handlePublishError(cid, false, false, true, 0);
        engine.handleCaptureEnded(cid, 0);
        QTest::qWait(200);
        QCOMPARE(failed.count(), 0);

        engine.handlePublishError(cid, false, false, true, 1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), cid);

        engine.stop();
        engine.start();
        engine.publishVideo(QStringLiteral("cid-next"), true, 0);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-next")));
        QVERIFY2(!engine.publishedBinPropertyForTest(
                            QStringLiteral("cid-next"),
                            QStringLiteral("capentry"), QStringLiteral("caps"))
                      .contains(QLatin1String("DMABuf")),
                 "a later share offered DMA-BUF again after it had failed");
        engine.stop();
    }

    void onlyAPipewireSrcThatKeepsItsFormatsIsOfferedDmaBuf()
    {
        QVERIFY(!SfuMediaEngine::pipewireSrcKeepsFormatOnRenegotiation(
            QStringLiteral("1.4.2")));
        QVERIFY(!SfuMediaEngine::pipewireSrcKeepsFormatOnRenegotiation(
            QStringLiteral("1.4.10")));
        QVERIFY(!SfuMediaEngine::pipewireSrcKeepsFormatOnRenegotiation(
            QStringLiteral("1.5.80")));
        QVERIFY(SfuMediaEngine::pipewireSrcKeepsFormatOnRenegotiation(
            QStringLiteral("1.5.81")));
        QVERIFY(SfuMediaEngine::pipewireSrcKeepsFormatOnRenegotiation(
            QStringLiteral("1.6.6")));
        QVERIFY(!SfuMediaEngine::pipewireSrcKeepsFormatOnRenegotiation(
            QString()));

        if (!SfuMediaEngine::missingGpuShareElement().isEmpty()
            || !SfuMediaEngine::gpuShareChainUsable())
            QSKIP("no usable GL chain, so the share never takes the GPU entry");
        SfuMediaEngine::clearShareDmaBufCaptureFailedForTest();
        const auto restore = qScopeGuard(
            [] { SfuMediaEngine::setPipewirePluginVersionForTest(QString()); });
        const auto entryFor = [](const QString &version) {
            SfuMediaEngine::setPipewirePluginVersionForTest(version);
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.setVideoSourceForTest(QStringLiteral(
                "appsrc is-live=true format=time "
                "caps=video/x-raw,format=BGRx,width=64,height=64,"
                "framerate=30/1"));
            engine.start();
            engine.publishVideo(QStringLiteral("cid-share"), true, 0);
            const QString caps = engine.publishedBinPropertyForTest(
                QStringLiteral("cid-share"), QStringLiteral("capentry"),
                QStringLiteral("caps"));
            engine.stop();
            return caps;
        };
        QVERIFY(entryFor(QStringLiteral("1.6.6"))
                    .contains(QLatin1String("DMABuf")));
        QVERIFY2(!entryFor(QStringLiteral("1.4.10"))
                      .contains(QLatin1String("DMABuf")),
                 "pipewiresrc 1.4 was offered DMA-BUF");
    }

    void aSystemMemoryShareThatCannotNegotiateRetriesInDmaBufOnce()
    {
        if (!SfuMediaEngine::missingGpuShareElement().isEmpty()
            || !SfuMediaEngine::gpuShareChainUsable())
            QSKIP("no usable GL chain, so the share never takes the GPU entry");
        SfuMediaEngine::clearShareDmaBufCaptureFailedForTest();
        SfuMediaEngine::setPipewirePluginVersionForTest(QStringLiteral("1.4.10"));
        const auto restore = qScopeGuard([] {
            SfuMediaEngine::clearShareDmaBufCaptureFailedForTest();
            SfuMediaEngine::setPipewirePluginVersionForTest(QString());
        });
        const QString cid = QStringLiteral("cid-share");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(QStringLiteral(
            "appsrc is-live=true format=time "
            "caps=video/x-raw,format=BGRx,width=64,height=64,"
            "framerate=30/1"));
        QSignalSpy failed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        engine.publishVideo(cid, true, 0);
        const auto entry = [&engine](const QString &id) {
            return engine.publishedBinPropertyForTest(
                id, QStringLiteral("capentry"), QStringLiteral("caps"));
        };
        QVERIFY(!entry(cid).isEmpty());
        QVERIFY(!entry(cid).contains(QLatin1String("DMABuf")));

        engine.handlePublishError(cid, false, false, true, 0);
        QCOMPARE(failed.count(), 0);
        QTRY_VERIFY(entry(cid).contains(QLatin1String("DMABuf")));
        QVERIFY(!SfuMediaEngine::shareDmaBufCaptureFailedForTest());

        engine.handlePublishError(cid, false, false, true, 1);
        QCOMPARE(failed.count(), 1);

        engine.stop();
        engine.start();
        engine.publishVideo(QStringLiteral("cid-next"), true, 0);
        QVERIFY2(entry(QStringLiteral("cid-next"))
                     .contains(QLatin1String("DMABuf")),
                 "a later share started in system memory after it had failed");
        engine.stop();
    }

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

    void aBusErrorFromALiveOrUnknownBinIsNotAPublishFailure()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy publish(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        engine.handlePublishError(QStringLiteral("wb-pub"));
        engine.handlePublishError(QStringLiteral("never-published"));
        QCOMPARE(publish.count(), 0);

        engine.publishVideo(QStringLiteral("cid-video"),false,
-1);
        QTest::qWait(400);
        engine.handlePublishError(QStringLiteral("cid-video"));
        QCOMPARE(publish.count(), 0);

        engine.stop();
    }

    void theVolumeCurveIsLiteralBelowUnityAndExpandsAbove()
    {
        QCOMPARE(SfuMediaEngine::audioFactorPercent(0), 0);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(1), 1);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(50), 50);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(99), 99);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(100), 100);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(200), 1000);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(150), 550);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(101), 109);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(-5), 0);
        QCOMPARE(SfuMediaEngine::audioFactorPercent(10000), 1000);
    }

    void theOfferAfterUnpublishNoLongerAdvertisesTheTrack()
    {
        SfuMediaEngine engine;
        SfuMediaEngine receiver;
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(engine, receiver, this, &failure);
        engine.setTestSourceMode(true);
        QSignalSpy offers(&engine, &SfuMediaEngine::localDescription);
        engine.start();
        receiver.start();
        engine.publishVideo(QStringLiteral("cid-share"),false,
-1);
        QTest::qWait(400);

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
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        engine.stop();
        receiver.stop();
    }

    void stoppingAPublishRetiresItsTransceiver()
    {
        SfuMediaEngine engine;
        SfuMediaEngine receiver;
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(engine, receiver, this, &failure);
        engine.setTestSourceMode(true);
        engine.start();
        receiver.start();
        QCOMPARE(engine.publisherTrackSlotsForTest(), -1);

        engine.publishVideo(QStringLiteral("cid-share"),false,
-1);
        QTest::qWait(400);
        QCOMPARE(engine.publisherTrackSlotsForTest(), 1);

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

        engine.publishVideo(QStringLiteral("cid-share-2"),
false,-1);
        QTest::qWait(400);
        QCOMPARE(engine.publisherTrackSlotsForTest(), 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        engine.stop();
        receiver.stop();
    }

    void unpublishingALiveVideoBinDoesNotDeadlock()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-video"),false,
-1);
        QTest::qWait(400);
        engine.unpublish(QStringLiteral("cid-video"));
        engine.publishVideo(QStringLiteral("cid-video"),false,
-1);
        QTest::qWait(200);
        engine.stop();
    }

    void thePipelineBusesAreNotLeftToAccumulate()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-audio"));
        engine.publishVideo(QStringLiteral("cid-video"),false,
-1);
        QTest::qWait(600);
        QVERIFY2(engine.busesWithPendingMessagesForTest() == 0,
                 "a pipeline bus is holding messages nobody will ever read: "
                 "the sync handler is passing them into the async queue and "
                 "each one pins a reference on the element that posted it");
        engine.stop();
    }

    void anEngineDestroyedMidTeardownLeavesNothingRunning()
    {
        std::shared_ptr<const std::atomic<int>> outstanding;
        int armed = 0;
        {
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.start();
            engine.publishVideo(QStringLiteral("cid-video"),
false,-1);
            engine.publishAudio(QStringLiteral("cid-audio"));
            QTRY_VERIFY_WITH_TIMEOUT(engine.microphoneRtpOutForTest() > 0, 5000);
            outstanding = engine.teardownCounterForTest();
            engine.unpublish(QStringLiteral("cid-video"));
            engine.unpublish(QStringLiteral("cid-audio"));
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
        QTest::qWait(200);
        QCOMPARE(outstanding->load(), 0);
    }

    void aTeardownCompletionFromAClosedSessionLeavesTheNextAlone()
    {
        int fds[2] = { -1, -1 };
        QCOMPARE(::pipe(fds), 0);
        const QString cid = QStringLiteral("cid-share");

        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishVideo(cid,false,-1);
        QTest::qWait(400);
        engine.unpublish(cid);
        engine.stop();

        engine.start();
        engine.publishVideo(cid,false,-1, fds[0]);
        QVERIFY2(::fcntl(fds[0], F_GETFD) != -1,
                 "the new share's descriptor was closed before the stale "
                 "completion could even land, so this case proves nothing");

        QTest::qWait(400);
        QVERIFY2(::fcntl(fds[0], F_GETFD) != -1,
                 "a teardown completion from the previous call closed the "
                 "new share's PipeWire descriptor: the capture dies and "
                 "nothing says why");
        engine.stop();
        ::close(fds[1]);
    }

    void aPublishThatCannotLinkLeavesNothingRegistered()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        engine.start();
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

        engine.publishShareAudio(QStringLiteral("cid-share-audio"));
        QTest::qWait(300);
        QVERIFY2(engine.hasPublishedBinForTest(
                     QStringLiteral("cid-share-audio")),
                 "share audio could not be republished under the cid its own "
                 "failure had taken");
        QCOMPARE(engine.publisherTrackSlotsForTest(), 2);
        engine.stop();
    }

    void theShareSoundMutesOnItsOwnValveAndARebuildKeepsIt()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-mic"));
        engine.publishShareAudio(QStringLiteral("cid-share-audio"));
        QTRY_VERIFY(engine.hasPublishedBinForTest(
            QStringLiteral("cid-share-audio")));
        const QString valve = lightning::shareaudio::shareValveElementName();
        const auto drop = [&engine](const QString &cid, const QString &name) {
            return engine.publishedBinPropertyForTest(cid, name,
                                                      QStringLiteral("drop"));
        };
        QCOMPARE(drop(QStringLiteral("cid-share-audio"), valve),
                 QStringLiteral("false"));

        engine.setShareAudioMuted(true);
        QCOMPARE(drop(QStringLiteral("cid-share-audio"), valve),
                 QStringLiteral("true"));
        QVERIFY2(drop(QStringLiteral("cid-mic"), QStringLiteral("micvalve"))
                     == QStringLiteral("false"),
                 "muting the share's sound closed the microphone's valve");

        engine.unpublish(QStringLiteral("cid-share-audio"));
        engine.publishShareAudio(QStringLiteral("cid-share-audio-2"));
        QTRY_VERIFY(engine.hasPublishedBinForTest(
            QStringLiteral("cid-share-audio-2")));
        QVERIFY2(drop(QStringLiteral("cid-share-audio-2"), valve)
                     == QStringLiteral("true"),
                 "a share track rebuilt while muted came up audible");
        engine.setShareAudioMuted(false);
        QCOMPARE(drop(QStringLiteral("cid-share-audio-2"), valve),
                 QStringLiteral("false"));
        engine.stop();
    }

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
true,2147483,
-1);
        for (int i = 0; i < 100 && publish.count() == 0; ++i)
            QTest::qWait(100);
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
        QTest::qWait(600);
        QCOMPARE(publish.count(), 1);
        engine.stop();
    }

    void theRateStageNeverCropsTheCapture()
    {
        for (bool screenShare : {false, true}) {
            const QString stage =
                SfuMediaEngine::videoRateStage(screenShare);
            const QString desc =
                QStringLiteral(
                    "videotestsrc num-buffers=2 "
                    "! video/x-raw,width=3840,height=2160,framerate=30/1 "
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

    void theOpeningHoldIsStillPresentAndUnfixed()
    {
        const int frames =
            framesFromASingleCaptureBuffer(QStringLiteral("videorate"), 800);
        QVERIFY2(frames >= 0, "the videorate harness did not run");
        QCOMPARE(frames, 0);
    }

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
            QCOMPARE(desc.count(limits), 1);
        }
    }

    void theBlockedBadgeNeedsARateNotABadFrameOrARun()
    {
        using Policy = SfuMediaEngine::BlockedRunPolicy;

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

        {
            Policy p;
            bool raise = false;
            bool raised = false;
            for (int i = 0; i < 600; ++i) {
                if (p.note((i % 50) != 0, &raise) && raise)
                    raised = true;
            }
            QVERIFY2(raised,
                     "a stream failing 98% of its frames never badged — that "
                     "is the consecutive-run hole, and it is worse than the "
                     "defect it replaced");
        }

        {
            Policy p;
            bool raise = false;
            int raisedAt = -1;
            for (int i = 0; i < Policy::kWindow; ++i) {
                if (p.note(true, &raise) && raise && raisedAt < 0)
                    raisedAt = i;
            }
            QCOMPARE(raisedAt, Policy::kMinObserved - 1);
        }

        {
            Policy p;
            bool raise = false;
            for (int i = 0; i < Policy::kWindow; ++i)
                p.note(true, &raise);
            QVERIFY(p.announced);
            bool cleared = false;
            for (int i = 0; i < Policy::kWindow; ++i) {
                if (p.note(false, &raise) && !raise)
                    cleared = true;
            }
            QVERIFY2(cleared, "the badge never came off a recovered stream");
            QVERIFY(!p.announced);
        }

        {
            Policy p;
            bool raise = false;
            for (int i = 0; i < Policy::kWindow; ++i)
                p.note(true, &raise);
            QVERIFY(p.announced);
            int moves = 0;
            for (int i = 0; i < Policy::kWindow * 4; ++i) {
                if (p.note((i % 2) == 0, &raise))
                    ++moves;
            }
            QCOMPARE(moves, 0);
            QVERIFY(p.announced);
        }

        QVERIFY(Policy::kClearPercent < Policy::kRaisePercent);
        QVERIFY(Policy::kMinObserved <= Policy::kWindow);
    }

    void everyLiveQueueIsBoundedAndLeaky()
    {
        struct Lane { const char *path; int expected; };
        const QList<Lane> lanes = {
            { SOURCE_DIR "/src/calls/SfuMediaEngine.cpp", 8 },
            { SOURCE_DIR "/src/calls/ShareAudioSources.cpp", 2 },
            { SOURCE_DIR "/src/calls/GstCallMediaBackend.cpp", 6 },
            { SOURCE_DIR "/src/calls/AudioDeviceTester.cpp", 1 },
        };

        static const QRegularExpression queueElement(QStringLiteral(
            "\\bqueue(?=\\s+(?:!|[a-z][a-z0-9-]*=))"));

        for (const Lane &lane : lanes) {
            QFile file(QString::fromUtf8(lane.path));
            QVERIFY2(file.open(QIODevice::ReadOnly), lane.path);
            QString code = QString::fromUtf8(file.readAll());
            QVERIFY(!code.isEmpty());
            code.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
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
                // The video receive queue feeds a depayloader chain from
                // ReceiveVideoCodecs.h.
                if (next.contains(QStringLiteral("rtpvp8depay"))
                    || (next.trimmed() == QLatin1String("%1")
                        && code.mid(m.capturedStart(), 320).contains(QStringLiteral(
                               "receivableVideoCodec(videoCodec).chain"))))
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

    void theAudioReceiveChainIsInstrumentedForLossAndConcealment()
    {
        QFile file(QStringLiteral(SOURCE_DIR "/src/calls/SfuMediaEngine.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString code = QString::fromUtf8(file.readAll());
        QCOMPARE(code.count(QStringLiteral("queue name=recvqueue")), 2);
        QCOMPARE(code.count(QStringLiteral("opusdec name=recvdec")), 2);
        QVERIFY(code.contains(QStringLiteral("\"overrun\"")));
        QVERIFY(code.contains(QStringLiteral("GstRTPPacketLost")));
        QVERIFY(code.contains(QStringLiteral("audio receive health stream=")));
        QVERIFY(code.contains(QStringLiteral("jitterbuffer ")));
    }

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
        SfuMediaEngine::OutputSink fallback;
        fallback.sink = QStringLiteral("autoaudiosink name=outsink");
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(QString(), &fallback),
                 QStringLiteral("autoaudiosink"));
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(QString(), nullptr),
                 QStringLiteral("autoaudiosink"));
        QCOMPARE(SfuMediaEngine::receiveSinkDescription(
                     QStringLiteral("client"), nullptr),
                 QStringLiteral("pulsesink name=recvsink"));
        QFile file(QStringLiteral(SOURCE_DIR "/src/calls/SfuMediaEngine.cpp"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString code = QString::fromUtf8(file.readAll());
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

    void theLiveBurstAtAStreamsStartNeverRaisesTheBadge()
    {
        using Policy = SfuMediaEngine::BlockedRunPolicy;
        Policy p;
        bool raise = false;
        int announcements = 0;
        for (int i = 0; i < 5074; ++i) {
            if (p.note(i < 10, &raise))
                ++announcements;
        }
        QCOMPARE(announcements, 0);
        QVERIFY(!p.announced);

        Policy fresh;
        for (int i = 0; i < 10; ++i)
            fresh.note(true, &raise);
        QVERIFY(!fresh.announced);
        QCOMPARE(fresh.failurePercent(), -1);
    }

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
        QVERIFY2(code.contains(QStringLiteral("pipewiresrc fd=")),
                 "the comment stripper ate the code");
    }

    void allAudioPayloadersUseTheSharedMtu()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishAudio(QStringLiteral("mtu-audio"));
        GstElement *bin = engine.publishedBinForTest(QStringLiteral("mtu-audio"));
        QVERIFY(bin);
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(bin));
        GValue item = G_VALUE_INIT;
        int checked = 0;
        unsigned int micMtu = 0;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            if (GST_IS_RTP_BASE_PAYLOAD(element)) {
                g_object_get(element, "mtu", &micMtu, nullptr);
                ++checked;
            }
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
        engine.stop();
        QCOMPARE(checked, 1);
        QCOMPARE(micMtu, lightning::rtp::kRtpPayloadMtu);

        const QString share = lightning::shareaudio::encodedTrackDescription(
            QStringLiteral("audiotestsrc"), 12345u, false);
        QVERIFY(share.contains(QStringLiteral("mtu=%1").arg(lightning::rtp::kRtpPayloadMtu)));
        GError *error = nullptr;
        bin = gst_parse_bin_from_description(share.toUtf8().constData(), TRUE, &error);
        QVERIFY2(!error, error ? error->message : "");
        QVERIFY(bin);
        auto *encoder = gst_bin_get_by_name(GST_BIN(bin), "shareaudioenc");
        QVERIFY(encoder);
        auto *src = gst_element_get_static_pad(encoder, "src");
        auto *peer = gst_pad_get_peer(src);
        auto *pay = gst_pad_get_parent_element(peer);
        unsigned int shareMtu = 0;
        g_object_get(pay, "mtu", &shareMtu, nullptr);
        gst_object_unref(pay);
        gst_object_unref(peer);
        gst_object_unref(src);
        gst_object_unref(encoder);
        gst_object_unref(bin);
        QCOMPARE(shareMtu, lightning::rtp::kRtpPayloadMtu);
    }

    void videoPacketsStayWithinTheRtpMtu_data()
    {
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<int>("extensionId");
        QTest::addColumn<unsigned int>("mtu");
        QTest::newRow("camera-clear") << false << 0 << lightning::rtp::kRtpPayloadMtu;
        QTest::newRow("share-encrypted") << true << 0 << lightning::rtp::kRtpPayloadMtu;
        QTest::newRow("encrypted-one-byte-extension") << true << 3 << lightning::rtp::kRtpPayloadMtu;
        QTest::newRow("encrypted-two-byte-extension") << true << 20 << lightning::rtp::kRtpPayloadMtu;
        QTest::newRow("custom-mtu-with-extension") << true << 3 << 400u;
    }

    void videoPacketsStayWithinTheRtpMtu()
    {
        QFETCH(bool, encrypted);
        QFETCH(int, extensionId);
        QFETCH(unsigned int, mtu);
        lightning::rtp::registerVp8Payloader();
        SfuMediaEngine engine;
        const QString description = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("videotestsrc num-buffers=1 pattern=snow"),
            QStringLiteral("identity"),
            QStringLiteral("video/x-raw,width=640,height=480,framerate=30/1"),
            QStringLiteral("vp8enc deadline=1"), QString(), 12345u,
            QStringLiteral("videoconvert ! videoscale"),
            SfuMediaEngine::captureEntryFilter(false));
        QVERIFY(description.contains(QStringLiteral("mtu=%1").arg(lightning::rtp::kRtpPayloadMtu)));
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            (description + QStringLiteral(" ! appsink name=packets sync=false"))
                .toUtf8().constData(), &error);
        QVERIFY2(!error, error ? error->message : "");
        QVERIFY(pipeline);
        auto pipelineGuard = std::shared_ptr<GstElement>(pipeline, [](GstElement *p) {
            gst_element_set_state(p, GST_STATE_NULL);
            gst_object_unref(p);
        });
        GstElement *pay = nullptr;
        GstIterator *it = gst_bin_iterate_elements(GST_BIN(pipeline));
        GValue element = G_VALUE_INIT;
        while (gst_iterator_next(it, &element) == GST_ITERATOR_OK) {
            auto *candidate = GST_ELEMENT(g_value_get_object(&element));
            if (GST_IS_RTP_BASE_PAYLOAD(candidate))
                pay = GST_ELEMENT(gst_object_ref(candidate));
            g_value_reset(&element);
        }
        g_value_unset(&element);
        gst_iterator_free(it);
        QVERIFY(pay);
        auto payGuard = std::shared_ptr<GstElement>(pay, gst_object_unref);
        unsigned int configuredMtu = 0;
        g_object_get(pay, "mtu", &configuredMtu, nullptr);
        QCOMPARE(configuredMtu, lightning::rtp::kRtpPayloadMtu);
        g_object_set(pay, "mtu", mtu, nullptr);
        if (extensionId) {
            GstRTPHeaderExtension *ext = gst_rtp_header_extension_create_from_uri(
                "urn:ietf:params:rtp-hdrext:sdes:mid");
            QVERIFY(ext);
            gst_rtp_header_extension_set_id(ext, extensionId);
            g_object_set(ext, "mid", "video", nullptr);
            g_signal_emit_by_name(pay, "add-extension", ext);
            gst_object_unref(ext);
        }
        const QByteArray key(32, 'k');
        engine.setOutboundKey(0, key);
        engine.setEncryptionRequired(encrypted);
        auto *enc = gst_bin_get_by_name(GST_BIN(pipeline), "videoenc");
        QVERIFY(enc);
        auto *pad = gst_element_get_static_pad(enc, "src");
        if (encrypted)
            engine.installEncryptProbeForTest(pad, true);
        gst_object_unref(pad);
        gst_object_unref(enc);
        auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "packets");
        QVERIFY(sink);
        auto sinkGuard = std::shared_ptr<GstElement>(sink, gst_object_unref);
        QVERIFY(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
        int packets = 0;
        gsize largest = 0;
        bool marker = false;
        bool sawExtension = false;
        QByteArray frame;
        for (int i = 0; i < 4096 && !marker; ++i) {
            GstSample *sample = nullptr;
            g_signal_emit_by_name(sink, "try-pull-sample", GstClockTime(3 * GST_SECOND), &sample);
            if (!sample)
                break;
            auto *buffer = gst_sample_get_buffer(sample);
            largest = std::max(largest, gst_buffer_get_size(buffer));
            GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
            QVERIFY(gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp));
            sawExtension |= gst_rtp_buffer_get_extension(&rtp);
            const auto *payload = static_cast<const char *>(gst_rtp_buffer_get_payload(&rtp));
            frame.append(payload + 4, gst_rtp_buffer_get_payload_len(&rtp) - 4);
            marker = gst_rtp_buffer_get_marker(&rtp);
            gst_rtp_buffer_unmap(&rtp);
            gst_sample_unref(sample);
            ++packets;
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        QVERIFY(marker);
        QVERIFY(packets > 2);
        QVERIFY(frame.size() > 10000);
        QCOMPARE(sawExtension, extensionId != 0);
        QVERIFY2(largest <= mtu, qPrintable(QStringLiteral("largest RTP packet %1 exceeds MTU %2")
                                            .arg(largest).arg(mtu)));
        qInfo() << "largest RTP packet" << largest << "budget" << mtu << "packets" << packets;
        if (encrypted) {
            QCOMPARE(engine.framesEncrypted(), 1u);
            CallFrameCryptor receiver;
            receiver.setKey(0, key);
            QVERIFY(!receiver.decryptFrame(frame, CallFrameCryptor::FrameKind::VideoKey).isEmpty());
        }
    }

    void theScreenSharePipelineParsesIncludingItsSelfView()
    {
        lightning::rtp::registerVp8Payloader();
        const QString selfView = SfuMediaEngine::selfViewBranch();
        const QString description = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("videotestsrc is-live=true"),
            SfuMediaEngine::videoRateStage(true),
            QStringLiteral("video/x-raw,width=(int)[1,1920],"
                           "height=(int)[1,1080],"
                           "framerate=(fraction)[0/1,30/1]"),
            QStringLiteral("vp8enc deadline=1 end-usage=cbr "
                           "target-bitrate=3000000"),
            selfView, 12345u,
            QStringLiteral("videoconvert ! videoscale"),
                SfuMediaEngine::captureEntryFilter(false));
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
        GstElement *preview = gst_bin_get_by_name(GST_BIN(bin), "selfvidsink");
        QVERIFY2(preview, "the self-view branch is missing from the share");
        gst_object_unref(preview);
        gst_object_unref(bin);
    }

    void aReceivedTrackIsAttributedByThePadNotByItsIndex()
    {
        const QString pane = QString::fromUtf8(SOURCE_UNDER_TEST);
        QVERIFY(!pane.isEmpty());
        QVERIFY2(!pane.contains(QStringLiteral("g_object_get(srcPad, \"msid\"")),
                 "received tracks are attributed by a webrtcbin pad property");
        QVERIFY2(pane.contains(QStringLiteral("g_object_get(transceiver, \"mid\"")),
                 "the track's mid does not come from its own transceiver");
        QVERIFY2(!pane.contains(QStringLiteral("m_streamForMline.value(mline)")),
                 "a pad-name index still indexes the SDP section map");

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
        QVERIFY2(pane.contains(
                     QStringLiteral("trackSid = SfuMediaEngine::trackSidFromMsid")),
                 "the SDP scan records no per-section track sid, so the "
                 "fallback above has nothing to read");
        for (const char *map : { "m_streamForMline.clear()",
                                 "m_midForMline.clear()",
                                 "m_trackForMline.clear()" }) {
            QVERIFY2(pane.contains(QLatin1String(map)),
                     qPrintable(QStringLiteral(
                         "%1 is missing: a section map outlives its call")
                                    .arg(QLatin1String(map))));
        }
    }

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

        QVERIFY2(pane.contains(QStringLiteral(
                     "if (nodeId < 0 && windowHandle == 0 "
                     "&& !captureRect.isValid()) {")),
                 "the no-source refusal rejects a share that carries one of "
                 "the three source kinds but no node id");

        QVERIFY2(pane.contains(
                     QStringLiteral("wincap::registerWindowCaptureSrc()")),
                 "the window capture element is never registered");
    }

    void theLinuxNoPortalFallbackCapturesAnX11RootRectangle()
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QSKIP("the X11 fallback branch does not exist off Linux");
#else
        const QString source = SfuMediaEngine::screenShareSource(
1,-1,0,
            QRect(100, 50, 1280, 720));
        QVERIFY2(source.startsWith(QStringLiteral("ximagesrc")),
                 qPrintable(QStringLiteral("not an X11 capture: %1")
                                .arg(source)));
        QVERIFY2(!source.contains(QStringLiteral("pipewiresrc")),
                 "the fallback still names the portal's element, which is "
                 "the one thing this session does not have");
        QVERIFY(source.contains(QStringLiteral("startx=100")));
        QVERIFY(source.contains(QStringLiteral("starty=50")));
        QVERIFY2(source.contains(QStringLiteral("endx=1379")),
                 qPrintable(QStringLiteral("endx is not the inclusive right "
                                           "edge: %1").arg(source)));
        QVERIFY2(source.contains(QStringLiteral("endy=769")),
                 qPrintable(QStringLiteral("endy is not the inclusive bottom "
                                           "edge: %1").arg(source)));

        const QString atOrigin = SfuMediaEngine::screenShareSource(
            0, -1, 0, QRect(0, 0, 1920, 1080));
        QVERIFY(atOrigin.contains(QStringLiteral("endx=1919")));
        QVERIFY(atOrigin.contains(QStringLiteral("endy=1079")));

        const QString portal = SfuMediaEngine::screenShareSource(42, 7);
        QVERIFY(portal.startsWith(QStringLiteral("pipewiresrc")));
        QVERIFY(!portal.contains(QStringLiteral("ximagesrc")));
        QVERIFY(portal.contains(QStringLiteral("fd=7")));

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

    void theCaptureElementProbeAsksTheRealRegistry()
    {
        QCOMPARE(QLatin1String(SfuMediaEngine::x11ScreenCaptureElementName()),
                 QLatin1String("ximagesrc"));
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
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123|TR_xyz789 TR_xyz789")),
                 QStringLiteral("PA_abc123"));
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123 TR_xyz789")),
                 QStringLiteral("PA_abc123"));
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("PA_abc123")),
                 QStringLiteral("PA_abc123"));
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(
                     QStringLiteral("|TR_xyz789 TR_xyz789")),
                 QString());
        QCOMPARE(SfuMediaEngine::participantIdFromMsid(QString()), QString());

        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123|TR_xyz789 TR_xyz789")),
                 QStringLiteral("TR_xyz789"));
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123 TR_xyz789")),
                 QStringLiteral("TR_xyz789"));
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(
                     QStringLiteral("PA_abc123")),
                 QString());
        QCOMPARE(SfuMediaEngine::trackSidFromMsid(QString()), QString());
    }

    void aKeyAndAStreamIdConvergeOnOneRingInEitherOrder()
    {
        {
            SfuMediaEngine engine;
            const auto byIdentity =
                engine.recvCryptorFor(QStringLiteral("@a:s:DEV"));
            engine.noteParticipantIdentity(QStringLiteral("PA_1"),
                                           QStringLiteral("@a:s:DEV"));
            QCOMPARE(engine.recvCryptorFor(QStringLiteral("PA_1")).get(),
                     byIdentity.get());
        }
        {
            SfuMediaEngine engine;
            const auto byStream = engine.recvCryptorFor(QStringLiteral("PA_1"));
            engine.noteParticipantIdentity(QStringLiteral("PA_1"),
                                           QStringLiteral("@a:s:DEV"));
            QCOMPARE(engine.recvCryptorFor(QStringLiteral("@a:s:DEV")).get(),
                     byStream.get());
        }
        {
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

    void stoppingVideoAndShareAudioWaitsForTheOutstandingAnswer()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QSignalSpy failures(&sender, &SfuMediaEngine::failed);
        QSignalSpy receiveFailures(&receiver, &SfuMediaEngine::failed);
        QStringList offers;
        QStringList heldAnswers;
        bool holdAnswers = false;
        int answered = 0;
        connect(&sender, &SfuMediaEngine::localDescription, &receiver,
            [&](int target, const QString &kind, const QString &sdp) {
                if (target != int(SfuMediaEngine::Target::Publisher)
                    || kind != QStringLiteral("offer"))
                    return;
                offers << sdp;
                receiver.applyRemoteDescription(SfuMediaEngine::Target::Subscriber,
                                                kind, sdp);
            });
        connect(&receiver, &SfuMediaEngine::localDescription, &sender,
            [&](int target, const QString &kind, const QString &sdp) {
                if (target != int(SfuMediaEngine::Target::Subscriber)
                    || kind != QStringLiteral("answer"))
                    return;
                ++answered;
                if (holdAnswers)
                    heldAnswers << sdp;
                else
                    sender.applyRemoteDescription(SfuMediaEngine::Target::Publisher,
                                                  kind, sdp);
            });
        connect(&sender, &SfuMediaEngine::localCandidate, &receiver,
            [&](int target, const QString &candidate) {
                if (target == int(SfuMediaEngine::Target::Publisher))
                    receiver.applyRemoteCandidate(SfuMediaEngine::Target::Subscriber,
                                                  candidate);
            });
        connect(&receiver, &SfuMediaEngine::localCandidate, &sender,
            [&](int target, const QString &candidate) {
                if (target == int(SfuMediaEngine::Target::Subscriber))
                    sender.applyRemoteCandidate(SfuMediaEngine::Target::Publisher,
                                                candidate);
            });
        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("microphone"));
        QTRY_VERIFY_WITH_TIMEOUT(receiver.receiveBinForTest(QStringLiteral("microphone")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(answered > 0, 5000);
        QTest::qWait(150);
        int before = answered;
        sender.publishVideo(QStringLiteral("share-video"), false, -1);
        QTRY_VERIFY_WITH_TIMEOUT(answered > before, 5000);
        QTest::qWait(150);
        before = answered;
        sender.publishAudio(QStringLiteral("share-audio"));
        QTRY_VERIFY_WITH_TIMEOUT(answered > before, 5000);
        QTest::qWait(150);
        QVERIFY(failures.isEmpty());
        QVERIFY(receiveFailures.isEmpty());
        holdAnswers = true;
        const int initialOffers = offers.size();
        sender.unpublish(QStringLiteral("share-video"));
        sender.unpublish(QStringLiteral("share-audio"));
        QTRY_VERIFY_WITH_TIMEOUT(!heldAnswers.isEmpty(), 5000);
        QTest::qWait(300);
        QCOMPARE(offers.size(), initialOffers + 1);
        const QString answer = heldAnswers.takeFirst();
        holdAnswers = false;
        sender.applyRemoteDescription(SfuMediaEngine::Target::Publisher,
                                      QStringLiteral("answer"), answer);
        QTest::qWait(500);
        QVERIFY(failures.isEmpty());
        QVERIFY(receiveFailures.isEmpty());
        std::atomic<int> frames{0};
        GstElement *sink = firstSinkOf(receiver.receiveBinForTest(QStringLiteral("microphone")));
        QVERIFY(sink);
        countBuffersAt(sink, &frames);
        QTRY_VERIFY_WITH_TIMEOUT(frames.load() > 10, 5000);
        sender.stop();
        receiver.stop();
    }

    void localDescriptionCompletionIsCheckedAndSessionBound_data()
    {
        QTest::addColumn<QByteArray>("action");
        QTest::addColumn<bool>("subscriber");
        QTest::addColumn<bool>("restart");
        QTest::addColumn<QString>("category");
        QTest::newRow("local-offer-rejected") << QByteArray("set-local-description")
            << false << false << QStringLiteral("local_sdp_rejected");
        QTest::newRow("retired-local-completion") << QByteArray("set-local-description")
            << false << true << QString();
        QTest::newRow("offer-creation-rejected") << QByteArray("create-offer")
            << false << false << QStringLiteral("local_offer_failed");
        QTest::newRow("answer-creation-rejected") << QByteArray("create-answer")
            << true << false << QStringLiteral("local_answer_failed");
    }

    void localDescriptionCompletionIsCheckedAndSessionBound()
    {
        QFETCH(QByteArray, action);
        QFETCH(bool, subscriber);
        QFETCH(bool, restart);
        QFETCH(QString, category);
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("microphone"));
        QTRY_VERIFY_WITH_TIMEOUT(receiver.receiveBinForTest(QStringLiteral("microphone")), 10000);
        QTest::qWait(150);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        SfuMediaEngine *subject = subscriber ? &receiver : &sender;
        GstElement *bin = subscriber
            ? receiver.receiveBinForTest(QStringLiteral("microphone"))
            : sender.publishedBinForTest(QStringLiteral("microphone"));
        QVERIFY(bin);
        GstElement *peer = gst_bin_get_by_name(GST_BIN(GST_OBJECT_PARENT(bin)),
                                              subscriber ? "wb-sub" : "wb-pub");
        QVERIFY(peer);
        struct HeldReply {
            GstPromise *promise = nullptr;
            QByteArray action;
            ~HeldReply() {
                if (promise) {
                    gst_promise_interrupt(promise);
                    gst_promise_unref(promise);
                }
            }
        } held{nullptr, action};
        const gulong hold = g_signal_connect(peer, action.constData(),
            G_CALLBACK(+[](GstElement *peer, gpointer,
                           GstPromise *promise, gpointer data) {
                auto *held = static_cast<HeldReply *>(data);
                held->promise = gst_promise_ref(promise);
                g_signal_stop_emission_by_name(peer, held->action.constData());
            }), &held);
        const auto cleanup = qScopeGuard([&] {
            g_signal_handler_disconnect(peer, hold);
            gst_object_unref(peer);
        });
        QSignalSpy descriptions(subject, &SfuMediaEngine::localDescription);
        QSignalSpy failures(subject, &SfuMediaEngine::failed);
        sender.publishVideo(QStringLiteral("share-video"), false, -1);
        QTRY_VERIFY_WITH_TIMEOUT(held.promise, 5000);
        QTest::qWait(100);
        QCOMPARE(descriptions.size(), 0);
        if (restart) {
            subject->stop();
            subject->start();
        }
        GError *error = g_error_new_literal(GST_WEBRTC_ERROR,
            GST_WEBRTC_ERROR_INVALID_STATE, "fixture rejection");
        GstStructure *reply = gst_structure_new("fixture-error",
            "error", G_TYPE_ERROR, error, nullptr);
        g_error_free(error);
        gst_promise_reply(held.promise, reply);
        if (restart) {
            QTest::qWait(150);
            QVERIFY(failures.isEmpty());
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, 3000);
            QCOMPARE(failures.first().first().toString(), category);
        }
        QVERIFY(descriptions.isEmpty());
        sender.stop();
        receiver.stop();
    }

    void trickleWaitsForTheRemoteDescriptionPromise()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy descriptions(&engine, &SfuMediaEngine::localDescription);
        engine.start();
        engine.publishAudio(QStringLiteral("fixture"));
        QTRY_VERIFY_WITH_TIMEOUT(!descriptions.isEmpty(), 3000);
        GstElement *bin = engine.publishedBinForTest(QStringLiteral("fixture"));
        QVERIFY(bin);
        GstElement *peer = gst_bin_get_by_name(GST_BIN(GST_OBJECT_PARENT(bin)), "wb-pub");
        QVERIFY(peer);
        struct HeldReply {
            GstPromise *promise = nullptr;
            ~HeldReply() {
                if (promise) {
                    gst_promise_interrupt(promise);
                    gst_promise_unref(promise);
                }
            }
        } held;
        const gulong hold = g_signal_connect(peer, "set-remote-description",
            G_CALLBACK(+[](GstElement *peer, gpointer,
                           GstPromise *promise, gpointer data) {
                auto *held = static_cast<HeldReply *>(data);
                held->promise = gst_promise_ref(promise);
                g_signal_stop_emission_by_name(peer, "set-remote-description");
            }), &held);
        int added = 0;
        const gulong observe = g_signal_connect(peer, "add-ice-candidate",
            G_CALLBACK(+[](GstElement *, guint, const gchar *, gpointer data) {
                ++*static_cast<int *>(data);
            }), &added);
        const auto cleanup = qScopeGuard([&] {
            g_signal_handler_disconnect(peer, hold);
            g_signal_handler_disconnect(peer, observe);
            gst_object_unref(peer);
        });
        engine.applyRemoteDescription(SfuMediaEngine::Target::Publisher,
            QStringLiteral("answer"), descriptions.first().at(2).toString());
        QVERIFY(held.promise);
        engine.applyRemoteCandidate(SfuMediaEngine::Target::Publisher,
            QStringLiteral(R"({"candidate":"candidate:fixture 1 UDP 1 127.0.0.1 9 typ host","sdpMLineIndex":0})"));
        QCOMPARE(added, 0);
        gst_promise_reply(held.promise, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(added, 1, 1000);
        engine.stop();
    }

    void aRemoteAnswerWithoutAnOfferReportsNegotiationFailure()
    {
        SfuMediaEngine producer;
        producer.setTestSourceMode(true);
        QSignalSpy descriptions(&producer, &SfuMediaEngine::localDescription);
        producer.start();
        producer.publishAudio(QStringLiteral("fixture"));
        QTRY_VERIFY_WITH_TIMEOUT(!descriptions.isEmpty(), 3000);
        const QString sdp = descriptions.first().at(2).toString();
        QVERIFY(!sdp.isEmpty());
        SfuMediaEngine receiver;
        receiver.setTestSourceMode(true);
        receiver.start();
        QSignalSpy failures(&receiver, &SfuMediaEngine::failed);
        receiver.applyRemoteDescription(SfuMediaEngine::Target::Publisher,
                                        QStringLiteral("answer"), sdp);
        QTRY_COMPARE_WITH_TIMEOUT(failures.size(), 1, 3000);
        QCOMPARE(failures.first().first().toString(), QStringLiteral("remote_sdp_rejected"));
        producer.stop();
        receiver.stop();
    }

    void remoteDescriptionCompletionCannotEndAReplacementSession()
    {
        SfuMediaEngine producer;
        producer.setTestSourceMode(true);
        QSignalSpy descriptions(&producer, &SfuMediaEngine::localDescription);
        producer.start();
        producer.publishAudio(QStringLiteral("fixture"));
        QTRY_VERIFY_WITH_TIMEOUT(!descriptions.isEmpty(), 3000);
        SfuMediaEngine receiver;
        receiver.setTestSourceMode(true);
        receiver.start();
        QSignalSpy failures(&receiver, &SfuMediaEngine::failed);
        receiver.applyRemoteDescription(SfuMediaEngine::Target::Publisher,
            QStringLiteral("answer"), descriptions.first().at(2).toString());
        receiver.stop();
        receiver.start();
        QTest::qWait(150);
        QVERIFY(failures.isEmpty());
        producer.stop();
        receiver.stop();
    }

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
        engine.setIceServers(QVariantList{ QVariant(), QVariant(42) });
        engine.stop();
    }

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

        const int fresh = measure(0);
        QVERIFY2(fresh > 0, "the rate stage produced nothing at all");
        QVERIFY2(fresh < 60,
                 qPrintable(QStringLiteral("a fresh publish emitted %1 buffers "
                                           "for 1 s of input")
                                .arg(fresh)));

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
            push(0);
            QTest::qWait(200);

            if (inject) {
                GstPad *rateSink =
                    SfuMediaEngine::keepAliveInjectionPad(pipeline);
                if (!rateSink) {
                    gst_element_set_state(pipeline, GST_STATE_NULL);
                    gst_object_unref(pipeline);
                    delete counter;
                    return -2;
                }
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

    void theKeepAlivePtsNeverLeavesTheSourcesTimebase()
    {
        const quint64 sampled = 3 * (GST_SECOND / 30);
        const quint64 at = SfuMediaEngine::keepAlivePts(
            sampled,true,600,
0,false);
        QVERIFY2(GST_CLOCK_TIME_IS_VALID(at), "no timestamp was produced");
        QCOMPARE(at, sampled + 600 * GST_MSECOND);
        QVERIFY2(at < 174 * GST_SECOND,
                 qPrintable(QStringLiteral(
                                "the injected PTS is %1 ns for a source whose "
                                "own clock reads %2 ns — the keep-alive has "
                                "left the source's timebase and will strand "
                                "every zero-based capture")
                                .arg(at)
                                .arg(sampled)));

        const quint64 again = SfuMediaEngine::keepAlivePts(
            sampled, true, 600, at, true);
        QVERIFY2(again > at, "two injections produced a non-advancing PTS");

        QVERIFY2(!GST_CLOCK_TIME_IS_VALID(SfuMediaEngine::keepAlivePts(
                     0,false, 600, 0, false)),
                 "the keep-alive invented a timestamp for a source that "
                 "supplied none");
    }

    void theRealPublishDescriptionStillOffersAnInjectionPad()
    {
        const QString description = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("videotestsrc is-live=true"),
            SfuMediaEngine::videoRateStage(true),
            SfuMediaEngine::shareLimitsCaps(1080, 30),
            QStringLiteral("vp8enc deadline=1"), QString(), 1234u,
            SfuMediaEngine::shareScaleStage(1080,false),
            SfuMediaEngine::captureEntryFilter(false));
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
        GstPad *peer = gst_pad_get_peer(pad);
        QVERIFY2(peer,
                 "`vidrate` has no upstream peer at parse time, so the "
                 "keep-alive's sampling probe is never installed and a still "
                 "window publishes nothing — silently");
        gst_object_unref(peer);
        gst_object_unref(pad);
        gst_object_unref(bin);
    }

    void aOneByteDmaBufIsRewrappedAtItsRealSize()
    {
#if defined(Q_OS_LINUX) && defined(LIGHTNING_HAVE_GST_ALLOCATORS)
        const int width = 64, height = 32, stride = width * 4;
        const gsize frame = gsize(stride) * height;
        const int fd = memfd_create("lightning-rewrap-test", MFD_CLOEXEC);
        QVERIFY(fd >= 0);
        QVERIFY(ftruncate(fd, off_t(frame)) == 0);
        GstAllocator *alloc = gst_dmabuf_allocator_new();
        auto makeBuffer = [&](gsize size) {
            GstBuffer *b = gst_buffer_new();
            GstMemory *m = gst_dmabuf_allocator_alloc(alloc, dup(fd), size);
            gst_buffer_append_memory(b, m);
            gsize offsets[1] = {0};
            gint strides[1] = {stride};
            gst_buffer_add_video_meta_full(
                b, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRx, width,
                height, 1, offsets, strides);
            GST_BUFFER_PTS(b) = 1234;
            return b;
        };
        GstBuffer *shortBuf = makeBuffer(1);
        GstBuffer *fixed = SfuMediaEngine::rewrapShortDmaBuf(shortBuf);
        QVERIFY2(fixed, "a 1-byte DMA-BUF memory was not rewrapped");
        QCOMPARE(gst_buffer_n_memory(fixed), 1u);
        QCOMPARE(gst_memory_get_sizes(gst_buffer_peek_memory(fixed, 0),
                                      nullptr, nullptr), frame);
        QVERIFY(gst_buffer_get_video_meta(fixed));
        QCOMPARE(quint64(GST_BUFFER_PTS(fixed)), quint64(1234));
        gst_buffer_unref(shortBuf);
        QVERIFY(gst_buffer_get_parent_buffer_meta(fixed));
        gst_buffer_unref(fixed);

        {
            const gsize skew = 4096;
            QVERIFY(ftruncate(fd, off_t(frame + skew)) == 0);
            GstBuffer *b = gst_buffer_new();
            GstMemory *m = gst_dmabuf_allocator_alloc(alloc, dup(fd),
                                                      frame + skew);
            gst_memory_resize(m, gssize(skew), 1);
            gst_buffer_append_memory(b, m);
            gsize offsets[1] = {0};
            gint strides[1] = {stride};
            gst_buffer_add_video_meta_full(
                b, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRx, width,
                height, 1, offsets, strides);
            GstBuffer *moved = SfuMediaEngine::rewrapShortDmaBuf(b);
            QVERIFY2(moved, "a short DMA-BUF at a non-zero offset was not "
                            "rewrapped");
            gsize off = 0, maxsize = 0;
            const gsize size = gst_memory_get_sizes(
                gst_buffer_peek_memory(moved, 0), &off, &maxsize);
            QCOMPARE(off, skew);
            QCOMPARE(size, frame);
            QVERIFY(ftruncate(fd, off_t(frame + skew - 1)) == 0);
            GstBuffer *tooSmall = gst_buffer_new();
            GstMemory *tm = gst_dmabuf_allocator_alloc(alloc, dup(fd),
                                                       frame + skew - 1);
            gst_memory_resize(tm, gssize(skew), 1);
            gst_buffer_append_memory(tooSmall, tm);
            gst_buffer_add_video_meta_full(
                tooSmall, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRx,
                width, height, 1, offsets, strides);
            QVERIFY(!SfuMediaEngine::rewrapShortDmaBuf(tooSmall));
            gst_buffer_unref(tooSmall);
            gst_buffer_unref(moved);
            gst_buffer_unref(b);
            QVERIFY(ftruncate(fd, off_t(frame)) == 0);
        }

        GstBuffer *whole = makeBuffer(frame);
        QVERIFY(!SfuMediaEngine::rewrapShortDmaBuf(whole));
        gst_buffer_unref(whole);
        GstBuffer *plain = gst_buffer_new_allocate(nullptr, 1, nullptr);
        QVERIFY(!SfuMediaEngine::rewrapShortDmaBuf(plain));
        gst_buffer_unref(plain);
        gst_object_unref(alloc);
        close(fd);
#else
        QSKIP("needs Linux and gstreamer-allocators");
#endif
    }

    void aLiveShareTakesANewResolutionAndRateInPlace()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setVideoSourceForTest(QStringLiteral(
            "videotestsrc is-live=true pattern=smpte"));
        engine.start();
        engine.setShareQuality(1080, 30, 0);
        const QString cid = QStringLiteral("cid-live-quality");
        engine.publishVideo(cid,true,1);
        GstElement *bin = engine.publishedBinForTest(cid);
        QVERIFY(bin);
        GstElement *limits = gst_bin_get_by_name(GST_BIN(bin), "publimits");
        GstElement *enc = gst_bin_get_by_name(GST_BIN(bin), "videoenc");
        QVERIFY2(limits, "the share's limits capsfilter is not addressable");
        QVERIFY(enc);
        GstPad *pad = gst_element_get_static_pad(limits, "src");
        auto shape = [pad, limits]() {
            int n = 0, d = 1;
            if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
                gst_structure_get_fraction(gst_caps_get_structure(caps, 0),
                                           "framerate", &n, &d);
                gst_caps_unref(caps);
            }
            GstCaps *set = nullptr;
            g_object_get(limits, "caps", &set, nullptr);
            gchar *text = set ? gst_caps_to_string(set) : nullptr;
            const QString ceiling = QString::fromUtf8(text ? text : "");
            g_free(text);
            if (set)
                gst_caps_unref(set);
            return QString::number(d ? n / d : 0) + QLatin1Char('/')
                   + (ceiling.contains(QStringLiteral("height=(int)[ 1, 720 ]"))
                          ? QStringLiteral("720")
                          : ceiling.contains(
                                QStringLiteral("height=(int)[ 1, 1080 ]"))
                              ? QStringLiteral("1080")
                              : ceiling);
        };
        QTRY_COMPARE_WITH_TIMEOUT(shape(), QStringLiteral("30/1080"), 8000);
        gint before = 0;
        g_object_get(enc, "target-bitrate", &before, nullptr);
        engine.setShareQuality(720, 15, 0);
        QVERIFY(engine.applyShareQualityLive(cid));
        QTRY_COMPARE_WITH_TIMEOUT(shape(), QStringLiteral("15/720"), 8000);
        gint after = 0;
        g_object_get(enc, "target-bitrate", &after, nullptr);
        QVERIFY2(after < before, "the encoder bitrate did not follow the "
                                 "smaller picture and lower rate");
        QVERIFY(!engine.applyShareQualityLive(QStringLiteral("no-such-cid")));
        gst_object_unref(pad);
        gst_object_unref(limits);
        gst_object_unref(enc);
        engine.stop();
    }

    void fitIntoKeepsTheAspectAndNeverUpscales()
    {
        using lightning::wincap::fitInto;

        const auto brave = fitInto(3840, 2100, 1920, 1080);
        QCOMPARE(brave.width, 1920);
        QCOMPARE(brave.height, 1050);

        const auto small = fitInto(800, 600, 1920, 1080);
        QCOMPARE(small.width, 800);
        QCOMPARE(small.height, 600);

        const auto explorer = fitInto(1557, 1213, 4000, 4000);
        QCOMPARE(explorer.width, 1556);
        QCOMPARE(explorer.height, 1212);

        const auto tall = fitInto(1000, 4000, 1920, 1080);
        QCOMPARE(tall.height, 1080);
        QCOMPARE(tall.width, 270);

        for (int w = 1900; w <= 1940; ++w) {
            for (int h = 1070; h <= 1090; ++h) {
                const auto fit = fitInto(w, h, 1920, 1080);
                QVERIFY(fit.width <= 1920);
                QVERIFY(fit.height <= 1080);
                QVERIFY(fit.width % 2 == 0);
                QVERIFY(fit.height % 2 == 0);
            }
        }

        QCOMPARE(fitInto(0, 0, 1920, 1080).width, 0);
        QCOMPARE(fitInto(1920, 1080, 0, 0).width, 0);
        QCOMPARE(fitInto(-4, -4, 1920, 1080).height, 0);
    }

    void theGpuScaleStageIsWellFormedAndOptIn()
    {
        for (int h : { 720, 1080, 1440, 2160 }) {
            const QString cpu = SfuMediaEngine::shareScaleStage(h, false);
            QCOMPARE(cpu, QStringLiteral("videoconvertscale n-threads=4"));

            const QString gpu = SfuMediaEngine::shareScaleStage(h, true);
            QVERIFY2(gpu.startsWith(QStringLiteral("glupload")),
                     qPrintable(gpu));
            QVERIFY2(gpu.contains(QStringLiteral("glcolorconvert")),
                     qPrintable(QStringLiteral(
                         "no glcolorconvert: a DMA_DRM buffer reaches the "
                         "scaler unconverted and the share is black: %1")
                             .arg(gpu)));
            const QString entry = SfuMediaEngine::captureEntryFilter(true);
            QVERIFY2(!entry.contains(QStringLiteral("video/x-raw(ANY)")),
                     qPrintable(QStringLiteral(
                         "an (ANY) fallback re-admits block-linear DMA-BUF "
                         "and the share goes out black: %1").arg(entry)));
            QVERIFY2(entry.contains(QStringLiteral("memory:DMABuf"))
                         && entry.contains(QStringLiteral("drm-format=")),
                     qPrintable(QStringLiteral(
                         "the GPU entry filter no longer requires a linear "
                         "DMA-BUF: %1").arg(entry)));
            {
                GstCaps *filter = gst_caps_from_string(
                    "video/x-raw(memory:DMABuf),drm-format=(string){ AR24, "
                    "XR24 },format=(string)DMA_DRM");
                const int a = entry.indexOf(QLatin1Char('"')) + 1;
                const int b = entry.lastIndexOf(QLatin1Char('"'));
                GstCaps *ours = gst_caps_from_string(
                    entry.mid(a, b - a).toUtf8().constData());
                QVERIFY(ours);
                GstCaps *both = gst_caps_intersect(ours, filter);
                const bool matches = !gst_caps_is_empty(both);
                gst_caps_unref(both);
                gst_caps_unref(ours);
                gst_caps_unref(filter);
                QVERIFY2(matches, "the GPU entry filter does not intersect "
                                  "a bare linear XR24/AR24 DMA-BUF offer");
            }
            QVERIFY2(gpu.contains(QStringLiteral("texture-target=2D")),
                     qPrintable(QStringLiteral(
                         "no texture-target pin: an external-oes texture can "
                         "reach the scaler and the share goes out black: %1")
                             .arg(gpu)));
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
            QVERIFY2(gpu.contains(QStringLiteral("[1,%1]").arg(h)),
                     qPrintable(gpu));
            QVERIFY2(gpu.contains(QStringLiteral("[1,%1]").arg((h * 16) / 9)),
                     qPrintable(gpu));
            for (QChar c : gpu) {
                QVERIFY2(!c.isDigit() || (c >= QLatin1Char('0')
                                          && c <= QLatin1Char('9')),
                         "a non-ASCII digit reached the GL caps string");
            }
        }

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
#if defined(Q_OS_LINUX)
        QVERIFY2(SfuMediaEngine::shareScaleStage(1080, true)
                     .contains(QStringLiteral("glupload")),
                 "the GPU stage stopped using glupload on Linux");
        {
            const QString missing = SfuMediaEngine::missingGpuShareElement();
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

#if defined(Q_OS_LINUX)
        QVERIFY2(SfuMediaEngine::shareGpuScalingRequested(),
                 "the GPU share path is no longer attempted by default");
#endif
        QVERIFY2(SfuMediaEngine::firstMissingElement(
                     {QByteArrayLiteral("queue")}).isEmpty(),
                 "the probe reports a present element as missing");
        QCOMPARE(SfuMediaEngine::firstMissingElement(
                     {QByteArrayLiteral("queue"),
                      QByteArrayLiteral("lightning-no-such-element"),
                      QByteArrayLiteral("also-not-real")}),
                 QStringLiteral("lightning-no-such-element"));
        const QString missingNow = SfuMediaEngine::missingGpuShareElement();
        QVERIFY2(missingNow.isEmpty()
                     || missingNow.startsWith(QStringLiteral("gl")),
                 qPrintable(QStringLiteral(
                     "the availability probe answered something that is not "
                     "a GL element name: %1").arg(missingNow)));
    }

    void theCameraKeepsItsOwnScaleStageWhateverTheShareIsDoing()
    {
        const QString shareCpu = SfuMediaEngine::shareScaleStage(2160, false);
        const QString cameraStage = QStringLiteral("videoconvert ! videoscale");
        QVERIFY2(shareCpu != cameraStage,
                 "the share and camera CPU stages became identical, so this "
                 "case can no longer detect the camera being re-pointed");

        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(false, 2160),
                 cameraStage);
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(true, 2160), shareCpu);
        QCOMPARE(SfuMediaEngine::cpuFallbackScaleStage(false, 720),
                 SfuMediaEngine::cpuFallbackScaleStage(false, 2160));
    }

    void shareCapsAndBitrateAreRightAtEveryOfferedQuality()
    {
        struct Row { int h; int fps; int w; };
        const QList<Row> rows = {
            { 720, 15, 1280 },  { 720, 30, 1280 },  { 720, 60, 1280 },
            { 1080, 15, 1920 }, { 1080, 30, 1920 }, { 1080, 60, 1920 },
            { 1440, 15, 2560 }, { 1440, 30, 2560 }, { 1440, 60, 2560 },
            { 2160, 15, 3840 }, { 2160, 30, 3840 }, { 2160, 60, 3840 },
        };
        for (const Row &r : rows) {
            const QString caps = SfuMediaEngine::shareLimitsCaps(r.h, r.fps);

            QVERIFY2(caps.contains(QStringLiteral("width=(int)[1,%1]")
                                       .arg(r.w)),
                     qPrintable(QStringLiteral("wrong width range at %1p%2: "
                                               "%3").arg(r.h).arg(r.fps)
                                    .arg(caps)));
            QVERIFY2(caps.contains(QStringLiteral("height=(int)[1,%1]")
                                       .arg(r.h)),
                     qPrintable(caps));
            QVERIFY2(caps.contains(QStringLiteral("framerate=(fraction)%1/1")
                                       .arg(r.fps)),
                     qPrintable(caps));
            QVERIFY2(caps.contains(
                         QStringLiteral("pixel-aspect-ratio=(fraction)1/1")),
                     qPrintable(caps));
            for (QChar c : caps) {
                QVERIFY2(!c.isDigit() || (c >= QLatin1Char('0')
                                          && c <= QLatin1Char('9')),
                         "a non-ASCII digit reached the caps string");
            }

            const QString enc = SfuMediaEngine::shareEncoderStage(r.h, r.fps);
            QVERIFY2(enc.contains(QStringLiteral("keyframe-max-dist=%1")
                                      .arg(2 * r.fps)),
                     qPrintable(enc));
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

        QVERIFY(SfuMediaEngine::shareEncoderStage(1080, 30)
                    .contains(QStringLiteral("target-bitrate=3000000")));
        QVERIFY(SfuMediaEngine::shareLimitsCaps(1080, 30)
                    .contains(QStringLiteral("width=(int)[1,1920]")));
    }

    void aShareBitrateCapOnlyEverLowersTheBitrate()
    {
        static const QRegularExpression rate(
            QStringLiteral("target-bitrate=(\\d+)"));
        const auto bitrate = [&](int h, int fps, int cap) {
            const QRegularExpressionMatch m =
                rate.match(SfuMediaEngine::shareEncoderStage(h, fps, cap));
            return m.hasMatch() ? m.captured(1).toInt() : -1;
        };
        QCOMPARE(bitrate(1080, 30, 0), 3000000);
        QCOMPARE(SfuMediaEngine::shareEncoderStage(1080, 30),
                 SfuMediaEngine::shareEncoderStage(1080, 30, 0));
        QCOMPARE(bitrate(1080, 30, 1000), 1000000);
        QCOMPARE(bitrate(1080, 30, 500), 500000);
        QCOMPARE(bitrate(720, 15, 4500), bitrate(720, 15, 0));
        QVERIFY(SfuMediaEngine::shareEncoderStage(1080, 60, 2000)
                    .contains(QStringLiteral("keyframe-max-dist=120")));
        SfuMediaEngine engine;
        engine.setShareQuality(1080, 30, 1800);
        QCOMPARE(engine.shareBitrateCapKbps(), 2000);
        engine.setShareQuality(1080, 30, -4);
        QCOMPARE(engine.shareBitrateCapKbps(), 0);
        QCOMPARE(SfuMediaEngine::snapShareBitrateCap(0), 0);
    }

    void theCameraCeilingFollowsTheChoiceAndAutoIsToday()
    {
        const QString today = QStringLiteral(
            "video/x-raw,width=(int)[1,1280],height=(int)[1,720],"
            "framerate=(fraction)30/1,pixel-aspect-ratio=(fraction)1/1");
        QCOMPARE(SfuMediaEngine::cameraLimitsCaps(false), today);
        QCOMPARE(SfuMediaEngine::cameraLimitsCaps(false, 720), today);
        QVERIFY(SfuMediaEngine::cameraEncoderStage(720)
                    .contains(QStringLiteral("target-bitrate=1700000")));

        struct Case { int h; int w; int bitrate; };
        const Case cases[] = { { 360, 640, 700000 }, { 480, 853, 1100000 },
                               { 1080, 1920, 3000000 } };
        for (const Case &c : cases) {
            const QString caps = SfuMediaEngine::cameraLimitsCaps(false, c.h);
            QVERIFY2(caps.contains(QStringLiteral("width=(int)[1,%1]")
                                       .arg(c.w))
                         && caps.contains(QStringLiteral("height=(int)[1,%1]")
                                              .arg(c.h))
                         && caps.contains(QStringLiteral("framerate=(fraction)30/1"))
                         && caps.contains(QStringLiteral(
                                "pixel-aspect-ratio=(fraction)1/1")),
                     qPrintable(caps));
            QVERIFY(SfuMediaEngine::cameraEncoderStage(c.h)
                        .contains(QStringLiteral("target-bitrate=%1")
                                      .arg(c.bitrate)));
            QVERIFY(!SfuMediaEngine::cameraLimitsCaps(true, c.h)
                         .contains(QStringLiteral("framerate")));
            const QString description = SfuMediaEngine::videoPipelineDescription(
                QStringLiteral("videotestsrc is-live=true"),
                SfuMediaEngine::cameraRateStage(false), caps,
                SfuMediaEngine::cameraEncoderStage(c.h), QString(), 1u,
                QStringLiteral("videoconvert ! videoscale"),
                SfuMediaEngine::captureEntryFilter(false));
            QVERIFY2(description.contains(caps), qPrintable(description));
        }
        QCOMPARE(SfuMediaEngine::snapCameraHeight(999), 720);
        QCOMPARE(SfuMediaEngine::snapCameraHeight(0), 720);
        SfuMediaEngine engine;
        QCOMPARE(engine.cameraMaxHeight(), 720);
        engine.setCameraQuality(480);
        QCOMPARE(engine.cameraMaxHeight(), 480);
    }

    void theVoiceSwitchesReachWebrtcdspAndTheReceiveBins()
    {
        using calls::noise::Mode;
        const QString today =
            calls::noise::voiceProcessingDescription(Mode::Off, true, false);
        QVERIFY(today.contains(QStringLiteral("echo-cancel=false")));
        QVERIFY(today.contains(QStringLiteral("gain-control=true")));
        QCOMPARE(calls::noise::voiceProcessingDescription(Mode::Off, true,
                                                          false, false, true),
                 today);
        const QString echoOn = calls::noise::voiceProcessingDescription(
            Mode::Off, true, false, true, true);
        QVERIFY(echoOn.contains(QStringLiteral("echo-cancel=true")));
        QVERIFY(echoOn.contains(QStringLiteral("gain-control=true")));
        const QString gainOff = calls::noise::voiceProcessingDescription(
            Mode::WebRtc, true, false, false, false);
        QVERIFY(gainOff.contains(QStringLiteral("gain-control=false")));
        QVERIFY(gainOff.contains(QStringLiteral("echo-cancel=false")));
        QVERIFY(gainOff.contains(QStringLiteral("noise-suppression=true")));
        const QString noDsp = calls::noise::voiceProcessingDescription(
            Mode::Off, false, false, true, false);
        QVERIFY(!noDsp.contains(QStringLiteral("webrtcdsp")));
        QVERIFY(!noDsp.contains(QStringLiteral("echo-cancel")));
        QVERIFY(!noDsp.contains(QStringLiteral("gain-control")));
        QCOMPARE(noDsp, calls::noise::voiceProcessingDescription(
                            Mode::Off, false, false, false, true));

        QVERIFY(SfuMediaEngine::echoProbeStage(false, true).isEmpty());
        QVERIFY(SfuMediaEngine::echoProbeStage(true, false).isEmpty());
        const QString probe = SfuMediaEngine::echoProbeStage(true, true);
        QVERIFY(probe.startsWith(QStringLiteral("! ")));
        QVERIFY(probe.endsWith(QLatin1Char(' ')));
        QVERIFY(probe.contains(QStringLiteral("webrtcechoprobe")));
        if (gst_init_check(nullptr, nullptr, nullptr)
            && calls::noise::webrtcDspAvailable()
            && SfuMediaEngine::echoProbeElementAvailable()) {
            const QString chain = QStringLiteral(
                "audiotestsrc num-buffers=1 ! audioconvert ! audioresample "
                "! volume %1! fakesink").arg(probe);
            GError *error = nullptr;
            GstElement *bin = gst_parse_launch(chain.toUtf8().constData(),
                                               &error);
            QVERIFY2(!error, error ? error->message : "");
            if (bin)
                gst_object_unref(bin);
        }
        SfuMediaEngine engine;
        QVERIFY(!engine.echoCancellation());
        QVERIFY(engine.autoGainControl());
        engine.setVoiceProcessing(true, false);
        QVERIFY(engine.echoCancellation());
        QVERIFY(!engine.autoGainControl());
    }

    void echoCancellationNeverStartsWithoutAPlaybackProbe()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !calls::noise::webrtcDspAvailable()
            || !SfuMediaEngine::echoProbeElementAvailable())
            QSKIP("webrtcdsp / webrtcechoprobe not available");
        SfuMediaEngine engine;
        engine.setVoiceProcessing(true, true);
        QVERIFY2(!engine.echoProbeLive(),
                 "no receive bin yet, so no probe may be reported");
        engine.setTestSourceMode(true);
        engine.start();
        const QString cid = QStringLiteral("cid-audio");
        engine.publishAudio(cid);
        const QString built = engine.lastAudioDescriptionForTest();
        QVERIFY2(built.contains(QLatin1String("echo-cancel=false")),
                 qPrintable(built));
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdsp"),
                     QStringLiteral("echo-cancel")),
                 QStringLiteral("false"));
        engine.engageEchoCancellation();
        QCOMPARE(engine.echoEngageAttemptsForTest(), 0);

        QVERIFY(SfuMediaEngine::echoProbeStage(true, true).contains(
            QLatin1String(SfuMediaEngine::kEchoProbeName)));
        GstElement *burn = gst_element_factory_make("webrtcechoprobe", nullptr);
        QVERIFY(burn);
        gst_object_unref(burn);

        const auto runDsp = [](const QString &probeStage) {
            const QString chain = QStringLiteral(
                "audiotestsrc num-buffers=20 ! audioconvert "
                "! audio/x-raw,format=S16LE,rate=48000,channels=1 "
                "! webrtcdsp echo-cancel=true ! fakesink name=dspsink "
                "audiotestsrc num-buffers=20 ! audioconvert %1! fakesink")
                .arg(probeStage);
            GError *error = nullptr;
            GstElement *pipeline = gst_parse_launch(chain.toUtf8().constData(),
                                                    &error);
            if (error)
                g_error_free(error);
            if (!pipeline)
                return QStringLiteral("parse failed");
            GstBus *bus = gst_element_get_bus(pipeline);
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            GstMessage *msg = gst_bus_timed_pop_filtered(
                bus, 3 * GST_SECOND,
                GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            QString result = QStringLiteral("timeout");
            if (msg) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                    result = QStringLiteral("eos");
                } else {
                    GError *e = nullptr;
                    gst_message_parse_error(msg, &e, nullptr);
                    result = QString::fromUtf8(e ? e->message : "error");
                    if (e)
                        g_error_free(e);
                }
                gst_message_unref(msg);
            }
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(bus);
            gst_object_unref(pipeline);
            return result;
        };
        QVERIFY2(runDsp(QString()).contains(QLatin1String("No echo probe")),
                 "webrtcdsp no longer fails without a probe; revisit the guard");
        const QString withProbe =
            runDsp(SfuMediaEngine::echoProbeStage(true, true));
        QVERIFY2(!withProbe.contains(QLatin1String("No echo probe")),
                 qPrintable(withProbe));
    }

    void echoCancellationEngagesOnARunningMicrophone()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !calls::noise::webrtcDspAvailable()
            || !SfuMediaEngine::echoProbeElementAvailable())
            QSKIP("webrtcdsp / webrtcechoprobe not available");
        const QString chain = QStringLiteral(
            "audiotestsrc is-live=true ! audioconvert "
            "! audio/x-raw,format=S16LE,rate=48000,channels=1 "
            "! webrtcdsp name=%1 echo-cancel=false noise-suppression=true "
            "gain-control=false ! audioconvert ! identity name=afterdsp "
            "! fakesink sync=false ")
            .arg(QLatin1String(calls::noise::kDspName));
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(chain.toUtf8().constData(),
                                                &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        GstElement *playback = gst_parse_bin_from_description(
            ("audiotestsrc is-live=true ! audioconvert "
             + SfuMediaEngine::echoProbeStage(true, true)
             + QStringLiteral("! fakesink sync=false"))
                .toUtf8()
                .constData(),
            FALSE, &error);
        QVERIFY2(playback && !error, error ? error->message : "no playback");

        GstBus *bus = gst_element_get_bus(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QVERIFY(gst_element_get_state(pipeline, nullptr, nullptr,
                                      3 * GST_SECOND)
                != GST_STATE_CHANGE_FAILURE);
        gst_bin_add(GST_BIN(pipeline), playback);
        gst_element_sync_state_with_parent(playback);

        const auto dspEcho = [&] {
            GstElement *dsp = gst_bin_get_by_name(GST_BIN(pipeline),
                                                  calls::noise::kDspName);
            gboolean on = FALSE;
            if (dsp) {
                g_object_get(dsp, "echo-cancel", &on, nullptr);
                gst_object_unref(dsp);
            }
            return on == TRUE;
        };
        QVERIFY(!dspEcho());
        QCOMPARE(calls::noise::engageEchoCancel(pipeline),
                 QStringLiteral("echo engaged"));
        QTRY_VERIFY(dspEcho());
        GstElement *after = gst_bin_get_by_name(GST_BIN(pipeline), "afterdsp");
        QVERIFY(after);
        std::atomic<int> buffers{0};
        GstPad *pad = gst_element_get_static_pad(after, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                          [](GstPad *, GstPadProbeInfo *, gpointer data) {
                              static_cast<std::atomic<int> *>(data)->fetch_add(1);
                              return GST_PAD_PROBE_OK;
                          },
                          &buffers, nullptr);
        QTRY_VERIFY(buffers.load() >= 10);
        GstMessage *message =
            gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        const bool errored = message != nullptr;
        if (message)
            gst_message_unref(message);
        gst_object_unref(pad);
        gst_object_unref(after);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        QVERIFY(!errored);
    }

    void echoEngageTheProbeRefusesLeavesTheMicrophoneRunning_data()
    {
        QTest::addColumn<bool>("heldByAnotherDsp");
        QTest::newRow("no probe") << false;
        QTest::newRow("probe already taken") << true;
    }

    void echoEngageTheProbeRefusesLeavesTheMicrophoneRunning()
    {
        QFETCH(bool, heldByAnotherDsp);
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !calls::noise::webrtcDspAvailable()
            || !SfuMediaEngine::echoProbeElementAvailable())
            QSKIP("webrtcdsp / webrtcechoprobe not available");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            QStringLiteral(
                "audiotestsrc is-live=true ! audioconvert "
                "! audio/x-raw,format=S16LE,rate=48000,channels=1 "
                "! webrtcdsp name=%1 echo-cancel=false noise-suppression=true "
                "gain-control=false ! audioconvert ! identity name=afterdsp "
                "! fakesink sync=false ")
                .arg(QLatin1String(calls::noise::kDspName))
                .toUtf8()
                .constData(),
            &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        GstElement *holder = nullptr;
        if (heldByAnotherDsp) {
            holder = gst_parse_launch(
                ("audiotestsrc is-live=true ! audioconvert "
                 + SfuMediaEngine::echoProbeStage(true, true)
                 + QStringLiteral(
                     "! fakesink sync=false audiotestsrc is-live=true "
                     "! audioconvert "
                     "! audio/x-raw,format=S16LE,rate=48000,channels=1 "
                     "! webrtcdsp echo-cancel=true ! fakesink sync=false"))
                    .toUtf8()
                    .constData(),
                &error);
            QVERIFY2(holder && !error, error ? error->message : "no holder");
            QVERIFY(gst_element_set_state(holder, GST_STATE_PLAYING)
                    != GST_STATE_CHANGE_FAILURE);
            QVERIFY(gst_element_get_state(holder, nullptr, nullptr,
                                          3 * GST_SECOND)
                    != GST_STATE_CHANGE_FAILURE);
        }
        GstBus *bus = gst_element_get_bus(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QVERIFY(gst_element_get_state(pipeline, nullptr, nullptr,
                                      3 * GST_SECOND)
                != GST_STATE_CHANGE_FAILURE);
        std::atomic<int> buffers{0};
        GstElement *after = gst_bin_get_by_name(GST_BIN(pipeline), "afterdsp");
        QVERIFY(after);
        GstPad *pad = gst_element_get_static_pad(after, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                          [](GstPad *, GstPadProbeInfo *, gpointer data) {
                              static_cast<std::atomic<int> *>(data)->fetch_add(1);
                              return GST_PAD_PROBE_OK;
                          },
                          &buffers, nullptr);
        QTRY_VERIFY(buffers.load() >= 5);

        std::atomic<int> refused{0};
        calls::noise::engageEchoCancel(pipeline, [&refused] {
            refused.fetch_add(1);
        });
        QTest::qWait(300);
        const int before = buffers.load();
        QTest::qWait(300);
        const int during = buffers.load() - before;

        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        QString failure;
        if (message) {
            GError *e = nullptr;
            gst_message_parse_error(message, &e, nullptr);
            failure = QString::fromUtf8(e ? e->message : "error");
            if (e)
                g_error_free(e);
            gst_message_unref(message);
        }
        GstElement *dsp = gst_bin_get_by_name(GST_BIN(pipeline),
                                              calls::noise::kDspName);
        gboolean echo = TRUE;
        if (dsp) {
            g_object_get(dsp, "echo-cancel", &echo, nullptr);
            gst_object_unref(dsp);
        }
        gst_object_unref(pad);
        gst_object_unref(after);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        if (holder) {
            gst_element_set_state(holder, GST_STATE_NULL);
            gst_object_unref(holder);
        }
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(during >= 5, "the microphone stopped");
        QVERIFY(!echo);
        QCOMPARE(refused.load(), 1);
    }

    void aReplacedDspKeepsTheProbeItsPredecessorHeld()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !calls::noise::webrtcDspAvailable()
            || !SfuMediaEngine::echoProbeElementAvailable())
            QSKIP("webrtcdsp / webrtcechoprobe not available");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            ("audiotestsrc is-live=true ! audioconvert "
             + SfuMediaEngine::echoProbeStage(true, true)
             + QStringLiteral(
                 "! fakesink sync=false audiotestsrc is-live=true "
                 "! audioconvert "
                 "! audio/x-raw,format=S16LE,rate=48000,channels=1 "
                 "! webrtcdsp name=%1 echo-cancel=true noise-suppression=true "
                 "gain-control=false ! audioconvert ! identity name=afterdsp "
                 "! fakesink sync=false")
                   .arg(QLatin1String(calls::noise::kDspName)))
                .toUtf8()
                .constData(),
            &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        GstBus *bus = gst_element_get_bus(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QVERIFY(gst_element_get_state(pipeline, nullptr, nullptr,
                                      3 * GST_SECOND)
                != GST_STATE_CHANGE_FAILURE);
        GstElement *first = gst_bin_get_by_name(GST_BIN(pipeline),
                                                calls::noise::kDspName);
        calls::noise::applyDspMode(pipeline, calls::noise::Mode::Off,
true,
true);
        const auto replaced = [&] {
            GstElement *dsp = gst_bin_get_by_name(GST_BIN(pipeline),
                                                  calls::noise::kDspName);
            const bool yes = dsp && dsp != first;
            if (dsp)
                gst_object_unref(dsp);
            return yes;
        };
        QTRY_VERIFY(replaced());
        GstElement *dsp = gst_bin_get_by_name(GST_BIN(pipeline),
                                              calls::noise::kDspName);
        gboolean echo = FALSE;
        g_object_get(dsp, "echo-cancel", &echo, nullptr);
        gst_object_unref(dsp);
        gst_object_unref(first);
        std::atomic<int> buffers{0};
        GstElement *after = gst_bin_get_by_name(GST_BIN(pipeline), "afterdsp");
        GstPad *pad = gst_element_get_static_pad(after, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                          [](GstPad *, GstPadProbeInfo *, gpointer data) {
                              static_cast<std::atomic<int> *>(data)->fetch_add(1);
                              return GST_PAD_PROBE_OK;
                          },
                          &buffers, nullptr);
        QTRY_VERIFY(buffers.load() >= 10);
        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        const bool errored = message != nullptr;
        if (message)
            gst_message_unref(message);
        gst_object_unref(pad);
        gst_object_unref(after);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        QVERIFY(echo);
        QVERIFY(!errored);
    }

    void aWebrtcDspThatFailsItsStartDoesNotSilenceTheMicrophone()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !calls::noise::webrtcDspAvailable()
            || !SfuMediaEngine::echoProbeElementAvailable())
            QSKIP("webrtcdsp / webrtcechoprobe not available");
        SfuMediaEngine engine;
        QSignalSpy failed(&engine, &SfuMediaEngine::echoCancellationFailed);
        QSignalSpy publishFailed(&engine, &SfuMediaEngine::publishFailed);
        QSignalSpy callFailed(&engine, &SfuMediaEngine::failed);
        engine.setVoiceProcessing(true, true);
        engine.setTestSourceMode(true);
        engine.start();
        engine.startNextMicrophoneWithEchoForTest();
        const QString cid = QStringLiteral("cid-audio");
        engine.publishAudio(cid);
        QVERIFY2(engine.lastAudioDescriptionForTest().contains(
                     QLatin1String("echo-cancel=true")),
                 "the fixture no longer reproduces the failure");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(engine.microphoneRtpOutForTest() >= 1, 5000);
        QVERIFY(engine.microphoneCapturedForTest() > 0);
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdsp"),
                     QStringLiteral("echo-cancel")),
                 QStringLiteral("false"));
        QTest::qWait(200);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(publishFailed.count(), 0);
        QCOMPARE(callFailed.count(), 0);
        const int attempts = engine.echoEngageAttemptsForTest();
        engine.engageEchoCancellation();
        QCOMPARE(engine.echoEngageAttemptsForTest(), attempts);
    }

    void aDspErrorWithoutEchoCancellationSaysNothingAboutEcho()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("GStreamer not available");
        SfuMediaEngine engine;
        QSignalSpy echoFailed(&engine, &SfuMediaEngine::echoCancellationFailed);
        QSignalSpy callFailed(&engine, &SfuMediaEngine::failed);
        QSignalSpy publishFailed(&engine, &SfuMediaEngine::publishFailed);
        engine.setTestSourceMode(true);
        engine.start();
        const QString cid = QStringLiteral("cid-audio");
        engine.publishAudio(cid);
        QVERIFY(engine.lastAudioDescriptionForTest().contains(
                    QLatin1String("echo-cancel=false"))
                || !engine.lastAudioDescriptionForTest().contains(
                    QLatin1String("echo-cancel=true")));
        engine.handleMicDspErrorForTest(cid, QStringLiteral("gain stage failed"));
        QTest::qWait(100);
        QCOMPARE(echoFailed.count(), 0);
        QCOMPARE(callFailed.count(), 0);
        QCOMPARE(publishFailed.count(), 1);
        QCOMPARE(publishFailed.at(0).at(0).toString(), cid);
        QCOMPARE(publishFailed.at(0).at(1).toString(),
                 QStringLiteral("audio_source_failed"));
        engine.handleMicDspErrorForTest(cid, QStringLiteral("again"));
        QCOMPARE(publishFailed.count(), 1);
    }

    void h264PayloaderKeepsTheByteTheKeyIndexLivesIn()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !SfuMediaEngine::firstMissingElement(
                    {QByteArrayLiteral("rtph264depay")}).isEmpty())
            QSKIP("rtph264depay not available");
        SfuMediaEngine engine;
        const QByteArray frame =
            QByteArray::fromHex("0000000167428028" "0000000168ce3c80"
                                "0000016588")
            + QByteArray(5000, char(0x55)) + QByteArray::fromHex("0c00");

        const auto through = [&](const char *payloader) {
            const QString desc = QStringLiteral(
                "appsrc name=in format=time caps=\"video/x-h264,"
                "stream-format=byte-stream,alignment=au\" "
                "! %1 pt=96 mtu=1200 ! rtph264depay "
                "! video/x-h264,stream-format=byte-stream,alignment=au "
                "! appsink name=out sync=false")
                .arg(QLatin1String(payloader));
            GError *error = nullptr;
            GstElement *pipeline =
                gst_parse_launch(desc.toUtf8().constData(), &error);
            if (error)
                g_error_free(error);
            if (!pipeline)
                return QByteArray();
            GstElement *in = gst_bin_get_by_name(GST_BIN(pipeline), "in");
            GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            GstBuffer *buffer = gst_buffer_new_memdup(frame.constData(),
                                                      frame.size());
            GST_BUFFER_PTS(buffer) = 0;
            GstFlowReturn flow = GST_FLOW_OK;
            g_signal_emit_by_name(in, "push-buffer", buffer, &flow);
            gst_buffer_unref(buffer);
            g_signal_emit_by_name(in, "end-of-stream", &flow);
            QByteArray result;
            GstSample *sample = nullptr;
            g_signal_emit_by_name(out, "try-pull-sample", 3 * GST_SECOND,
                                  &sample);
            if (sample) {
                GstMapInfo map;
                GstBuffer *got = gst_sample_get_buffer(sample);
                if (gst_buffer_map(got, &map, GST_MAP_READ)) {
                    result = QByteArray(
                        reinterpret_cast<const char *>(map.data), map.size);
                    gst_buffer_unmap(got, &map);
                }
                gst_sample_unref(sample);
            }
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(in);
            gst_object_unref(out);
            gst_object_unref(pipeline);
            return result;
        };
        const QByteArray ours =
            through(lightning::rtp::h264PayloaderName());
        QVERIFY2(ours.endsWith(QByteArray::fromHex("0c00")),
                 qPrintable(ours.right(4).toHex()));
        QCOMPARE(ours.right(5002), frame.right(5002));
        if (SfuMediaEngine::firstMissingElement(
                {QByteArrayLiteral("rtph264pay")}).isEmpty()) {
            QVERIFY(!through("rtph264pay").endsWith(QByteArray::fromHex("0c00")));
        }
    }

    void aSubscriberAnswerKeepsRetransmissionForVideo()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        const auto restore = qScopeGuard(
            [] { qunsetenv("LIGHTNING_RECEIVE_VIDEO_CODEC"); });
        qputenv("LIGHTNING_RECEIVE_VIDEO_CODEC", "vp8");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        QString offer = subscriberOffer({QList<int>{96}}, 1);
        offer.replace(QStringLiteral("m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"),
                      QStringLiteral("m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"));
        offer.replace(QStringLiteral("a=rtpmap:96 VP8/90000\r\n"),
                      QStringLiteral("a=rtpmap:96 VP8/90000\r\n"
                                     "a=rtcp-fb:96 nack\r\n"
                                     "a=rtcp-fb:96 nack pli\r\n"
                                     "a=rtpmap:97 rtx/90000\r\n"
                                     "a=fmtp:97 apt=96\r\n"
                                     "a=ssrc-group:FID 2000 3000\r\n"
                                     "a=ssrc:3000 cname:b\r\n"));
        QVERIFY(offer.contains(QLatin1String("rtx/90000")));
        const QString answer = answerTo(engine, offer);
        QVERIFY(!answer.isEmpty());
        const QStringList video = sdpSection(answer, 2);
        QVERIFY2(video.value(0).startsWith(QLatin1String("video ")), qPrintable(answer));
        QVERIFY2(sdpEncodings(video).contains(QStringLiteral("rtx")),
                 qPrintable(video.join(QLatin1Char('\n'))));
        QVERIFY2(video.contains(QStringLiteral("a=rtcp-fb:96 nack")),
                 qPrintable(video.join(QLatin1Char('\n'))));
        engine.stop();
    }

    void aPublishedCameraOffersRetransmission()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy offers(&engine, &SfuMediaEngine::localDescription);
        engine.start();
        engine.publishVideo(QStringLiteral("rtx-camera"), false, -1);
        QString sent;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto &emitted : std::as_const(offers)) {
                    if (emitted.at(1).toString() == QLatin1String("offer")
                        && emitted.at(2).toString().contains(QLatin1String("m=video")))
                        sent = emitted.at(2).toString();
                }
                return !sent.isEmpty();
            }(), 10000);
        const int video = sent.indexOf(QLatin1String("m=video"));
        const QString section = sent.mid(video);
        QVERIFY2(section.contains(QLatin1String("rtx/90000"))
                     && section.contains(QLatin1String("apt=96")),
                 qPrintable(section));
        QVERIFY2(section.contains(QLatin1String("a=ssrc-group:FID")), qPrintable(section));
        engine.stop();
    }

    void aFrameThatFailsToDecryptHoldsTheDeltasUntilAKeyframe()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        SfuMediaEngine engine;
        engine.setEncryptionRequired(true);
        engine.setInboundKey(QStringLiteral("held"), 0, QByteArray(32, 'k'));
        HeldVideoRig rig(engine, QStringLiteral("held"));
        CallFrameCryptor good, wrong;
        keyed(good, 0, 'k');
        keyed(wrong, 0, 'w');
        rig.push(good, true);
        rig.push(good, false);
        QCOMPARE(rig.delivered, 2);
        QCOMPARE(rig.requests, 0);
        rig.push(wrong, false);
        for (int i = 0; i < 3; ++i)
            rig.push(good, false);
        QCOMPARE(rig.delivered, 2);
        QCOMPARE(rig.requests, 1);
        rig.push(good, true);
        rig.push(good, false);
        QCOMPARE(rig.delivered, 4);
        QCOMPARE(rig.requests, 1);
    }

    void aViewerThatCannotDecryptAsksForNoKeyframes()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        SfuMediaEngine engine;
        engine.setEncryptionRequired(true);
        engine.setInboundKey(QStringLiteral("held"), 0, QByteArray(32, 'k'));
        HeldVideoRig unknownIndex(engine, QStringLiteral("held"));
        HeldVideoRig noKey(engine, QStringLiteral("keyless"));
        CallFrameCryptor other;
        keyed(other, 1, 'o');
        QElapsedTimer window;
        window.start();
        while (window.elapsed() < 1600) {
            unknownIndex.push(other, window.elapsed() % 500 < 40);
            noKey.push(other, false);
            QTest::qWait(30);
        }
        QCOMPARE(unknownIndex.delivered, 0);
        QCOMPARE(noKey.delivered, 0);
        QCOMPARE(unknownIndex.requests, 0);
        QCOMPARE(noKey.requests, 0);
    }

    void theDepayloadersKeyframeRequestsLeaveAsOnePliPer300Ms()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        struct Upstream { int requests = 0; int allHeaders = 0; } seen;
        GstPad *queueOut = gst_pad_new("src", GST_PAD_SRC);
        GstPad *depayIn = gst_pad_new("sink", GST_PAD_SINK);
        g_object_set_data(G_OBJECT(queueOut), "seen", &seen);
        gst_pad_set_event_function(queueOut, [](GstPad *pad, GstObject *, GstEvent *event) -> gboolean {
            auto *s = static_cast<Upstream *>(g_object_get_data(G_OBJECT(pad), "seen"));
            if (gst_video_event_is_force_key_unit(event)) {
                ++s->requests;
                gboolean all = FALSE;
                gst_video_event_parse_upstream_force_key_unit(event, nullptr, &all, nullptr);
                s->allHeaders += all ? 1 : 0;
            }
            gst_event_unref(event);
            return TRUE;
        });
        gst_pad_set_active(depayIn, TRUE);
        gst_pad_set_active(queueOut, TRUE);
        QCOMPARE(gst_pad_link(queueOut, depayIn), GST_PAD_LINK_OK);
        auto stats = std::make_shared<lightning::calls::StreamInfoStats>();
        SfuMediaEngine::installKeyframeRequestGate(depayIn, stats);
        const auto ask = [depayIn] {
            gst_pad_push_event(depayIn, gst_video_event_new_upstream_force_key_unit(
                                            GST_CLOCK_TIME_NONE, TRUE, 0));
        };
        for (int i = 0; i < 5; ++i)
            ask();
        QCOMPARE(seen.requests, 1);
        QTest::qWait(350);
        ask();
        ask();
        QCOMPARE(seen.requests, 2);
        QCOMPARE(seen.allHeaders, 0);
        QCOMPARE(stats->keyframeRequests.load(), 2ULL);
        gst_pad_set_active(queueOut, FALSE);
        gst_pad_set_active(depayIn, FALSE);
        gst_object_unref(queueOut);
        gst_object_unref(depayIn);
    }

    void aLostVideoPacketFreezesThePictureAndAsksForAKeyframe()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "videotestsrc is-live=true pattern=ball "
            "! video/x-raw,width=320,height=240,framerate=30/1 "
            "! vp8enc deadline=1 keyframe-max-dist=100000 "
            "! rtpvp8pay mtu=300 "
            "! application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96 "
            "! identity name=network",
            &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        QString why;
        GstElement *bin = SfuMediaEngine::buildReceiveBin(true, QString(), true, &why);
        QVERIFY2(bin, qPrintable(why));
        QVERIFY(gst_bin_add(GST_BIN(pipeline), bin));
        GstElement *network = gst_bin_get_by_name(GST_BIN(pipeline), "network");
        QVERIFY(gst_element_link(network, bin));

        struct Network {
            std::atomic<int> packets{0};
            std::atomic<bool> lose{false};
            std::atomic<int> keyframeRequests{0};
        } net;
        GstPad *out = gst_element_get_static_pad(network, "src");
        gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                auto *n = static_cast<Network *>(data);
                n->packets.fetch_add(1);
                return n->lose.exchange(false) ? GST_PAD_PROBE_DROP
                                               : GST_PAD_PROBE_OK;
            }, &net, nullptr);
        // Swallowed so the sender cannot answer: the picture must stay frozen.
        gst_pad_add_probe(out, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
            [](GstPad *, GstPadProbeInfo *info, gpointer data) {
                if (!gst_video_event_is_force_key_unit(GST_PAD_PROBE_INFO_EVENT(info)))
                    return GST_PAD_PROBE_OK;
                static_cast<Network *>(data)->keyframeRequests.fetch_add(1);
                return GST_PAD_PROBE_DROP;
            }, &net, nullptr);
        gst_object_unref(out);
        gst_object_unref(network);

        std::atomic<int> decoded{0};
        GstElement *sink = gst_bin_get_by_name(GST_BIN(bin), "vidsink");
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            }, &decoded, nullptr);
        gst_object_unref(sinkPad);
        gst_object_unref(sink);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QTRY_VERIFY_WITH_TIMEOUT(decoded.load() > 15, 10000);
        net.lose = true;
        QTRY_VERIFY_WITH_TIMEOUT(!net.lose.load(), 2000);
        QTest::qWait(300);
        const int afterLoss = decoded.load();
        QTest::qWait(1000);
        const int frozen = decoded.load() - afterLoss;
        const int requests = net.keyframeRequests.load();
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        QVERIFY2(requests > 0, "a lost packet asked the sender for nothing");
        QVERIFY2(frozen == 0,
                 qPrintable(QStringLiteral("%1 frames were decoded against a "
                                           "missing reference").arg(frozen)));
    }

    void aVp9StreamIsDepayloadedAndDecoded()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        if (!SfuMediaEngine::firstMissingElement(
                 {QByteArrayLiteral("vp9enc"), QByteArrayLiteral("rtpvp9pay")})
                 .isEmpty()
            || SfuMediaEngine::receivableVideoCodec(QStringLiteral("VP9")).encoding
                != QLatin1String("VP9"))
            QSKIP("no VP9 here");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            "videotestsrc is-live=true pattern=ball "
            "! video/x-raw,width=320,height=240,framerate=30/1 "
            "! vp9enc deadline=1 cpu-used=8 ! rtpvp9pay mtu=300 "
            "! application/x-rtp,media=video,encoding-name=VP9,clock-rate=90000,payload=98 "
            "! identity name=network",
            &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        QString why;
        GstElement *bin = SfuMediaEngine::buildReceiveBin(
            true, QString(), true, &why, QString(), nullptr, false,
            QStringLiteral("VP9"));
        QVERIFY2(bin, qPrintable(why));
        QVERIFY(gst_bin_add(GST_BIN(pipeline), bin));
        GstElement *network = gst_bin_get_by_name(GST_BIN(pipeline), "network");
        QVERIFY(gst_element_link(network, bin));
        gst_object_unref(network);
        GstElement *depay = gst_bin_get_by_name(GST_BIN(bin), "recvdepay");
        QVERIFY(depay);
        QCOMPARE(QByteArray(GST_OBJECT_NAME(gst_element_get_factory(depay))),
                 QByteArray("rtpvp9depay"));
        gst_object_unref(depay);

        std::atomic<int> decoded{0};
        GstElement *sink = gst_bin_get_by_name(GST_BIN(bin), "vidsink");
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *, gpointer data) {
                static_cast<std::atomic<int> *>(data)->fetch_add(1);
                return GST_PAD_PROBE_OK;
            }, &decoded, nullptr);
        gst_object_unref(sinkPad);
        gst_object_unref(sink);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        const bool played = QTest::qWaitFor([&] { return decoded.load() > 15; },
                                            10000);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        QVERIFY2(played, "a VP9 stream decoded nothing");
    }

    void aVp9FrameIsDecryptedWholeAndAVp8OneKeepsItsHeader()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        if (SfuMediaEngine::receivableVideoCodec(QStringLiteral("VP9")).encoding
            != QLatin1String("VP9"))
            QSKIP("no VP9 here");
        const QByteArray key(16, 'k');
        CallFrameCryptor sender;
        QVERIFY(sender.setKey(3, key));
        sender.setCurrentKeyIndex(3);
        const QByteArray frame(200, 'v');
        const QByteArray whole = sender.encryptFrame(
            frame, CallFrameCryptor::FrameKind::VideoWhole, 7, 1000);
        QVERIFY(!whole.isEmpty());
        QVERIFY(!whole.startsWith('v'));

        for (const QString &codec : {QStringLiteral("VP9"), QStringLiteral("VP8")}) {
            SfuMediaEngine engine;
            const QString stream = QStringLiteral("PA_") + codec;
            engine.setEncryptionRequired(true);
            engine.setInboundKey(stream, 3, key);
            DecryptProbeRig rig(engine, stream, true, codec);
            QVERIFY(rig.ok());
            rig.push(whole);
            QVERIFY(rig.drain());
            QCOMPARE(rig.passed(), codec == QLatin1String("VP9") ? 1 : 0);
        }
    }

    void anAnswerAdmitsOnlyWhatThisBuildDecodes_data()
    {
        QTest::addColumn<bool>("vp8Only");
        QTest::newRow("as built") << false;
        QTest::newRow("forced VP8") << true;
    }
    void anAnswerAdmitsOnlyWhatThisBuildDecodes()
    {
        QFETCH(bool, vp8Only);
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        const auto have = [](const QList<QByteArray> &names) {
            return SfuMediaEngine::firstMissingElement(names).isEmpty();
        };
        const bool h264 = !vp8Only && !SfuMediaEngine::h264DecoderElement().isEmpty()
            && have({QByteArrayLiteral("rtph264depay"),
                     QByteArrayLiteral("h264parse")});
        const bool vp9 = !vp8Only && have({QByteArrayLiteral("rtpvp9depay")})
            && (have({QByteArrayLiteral("vp9dec")})
                || have({QByteArrayLiteral("avdec_vp9")}));
        const bool av1 = !vp8Only && have({QByteArrayLiteral("rtpav1depay")})
            && (have({QByteArrayLiteral("dav1ddec")})
                || have({QByteArrayLiteral("av1dec")}));
        if (!vp8Only && !h264 && !vp9)
            QSKIP("this environment decodes nothing but VP8");
        const auto restore = qScopeGuard(
            [] { qunsetenv("LIGHTNING_RECEIVE_VIDEO_CODEC"); });
        if (vp8Only)
            qputenv("LIGHTNING_RECEIVE_VIDEO_CODEC", "vp8");
        else
            qunsetenv("LIGHTNING_RECEIVE_VIDEO_CODEC");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        QStringList wanted{QStringLiteral("VP8")};
        if (vp9)
            wanted << QStringLiteral("VP9");
        if (av1)
            wanted << QStringLiteral("AV1");
        if (h264)
            wanted << QStringLiteral("H264");
        const QList<int> all{96, 98, 45, 125};
        const auto admitted = [](const QString &answer, int section) {
            QStringList encodings = sdpEncodings(sdpSection(answer, section));
            encodings.sort();
            return encodings;
        };
        wanted.sort();

        const QString first = answerTo(engine, subscriberOffer({all}, 1));
        QVERIFY(!first.isEmpty());
        QVERIFY2(sdpSection(first, 0).value(0).startsWith(
                     QLatin1String("application 9 ")), qPrintable(first));
        QVERIFY2(sdpSection(first, 1).value(0).startsWith(
                     QLatin1String("audio 9 ")), qPrintable(first));
        QVERIFY(sdpEncodings(sdpSection(first, 1)).first()
                    .compare(QLatin1String("opus"), Qt::CaseInsensitive) == 0);
        QCOMPARE(admitted(first, 2), wanted);

        const QString second = answerTo(engine, subscriberOffer({all, all}, 2));
        QVERIFY(!second.isEmpty());
        QCOMPARE(admitted(second, 2), wanted);
        QCOMPARE(admitted(second, 3), wanted);

        QSignalSpy failed(&engine, &SfuMediaEngine::failed);
        const QString third = answerTo(
            engine, subscriberOffer({all, all, QList<int>{99}}, 3));
        QVERIFY2(third.isEmpty(), qPrintable(third));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(),
                 QStringLiteral("local_sdp_rejected"));
        engine.stop();
    }

    void onlyTheSfuReadsTheRewrittenOffer()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !SfuMediaEngine::firstMissingElement(
                    {QByteArrayLiteral("openh264enc"),
                     QByteArrayLiteral("h264parse")}).isEmpty())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        const auto restore = qScopeGuard([] { useStandInHardware(false); });
        useStandInHardware(true);
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy spy(&engine, &SfuMediaEngine::localDescription);
        engine.start();
        const QString cid = QStringLiteral("rewritten");
        engine.publishVideo(cid,true, 0);
        QString sent;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto &emitted : std::as_const(spy)) {
                    if (emitted.at(1).toString() == QLatin1String("offer")
                        && emitted.at(2).toString().contains(cid))
                        sent = emitted.at(2).toString();
                }
                return !sent.isEmpty();
            }(), 10000);
        QVERIFY2(sent.contains(QStringLiteral("a=ssrc:")) && sent.contains(
                     QStringLiteral(" msid:%1 %1\r\n").arg(cid)), qPrintable(sent));
        QVERIFY2(sent.contains(QStringLiteral("SAVPF 102 96\r\n")), qPrintable(sent));
        QVERIFY2(sent.contains(QStringLiteral("SAVPF 96 102\r\n")), qPrintable(sent));

        GstWebRTCSessionDescription *local = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                if (local)
                    gst_webrtc_session_description_free(local);
                local = nullptr;
                g_object_get(engine.m_publisher.webrtc, "local-description",
                             &local, nullptr);
                return local != nullptr;
            }(), 5000);
        gchar *text = gst_sdp_message_as_text(local->sdp);
        const QString kept = QString::fromUtf8(text);
        g_free(text);
        gst_webrtc_session_description_free(local);
        QVERIFY(kept.contains(cid));
        QVERIFY2(!kept.contains(QStringLiteral(" msid:%1 %1\r\n").arg(cid)), qPrintable(kept));
        QVERIFY2(!kept.contains(QLatin1String("SAVPF 102 96")), qPrintable(kept));
        QVERIFY2(kept.contains(QLatin1String("webrtctransceiver")), qPrintable(kept));
        engine.stop();
    }

    void aBackupThatCannotStartLatchesSoftwareAndReportsLater()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !SfuMediaEngine::firstMissingElement(
                    {QByteArrayLiteral("openh264enc"),
                     QByteArrayLiteral("h264parse")}).isEmpty())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        const auto restore = qScopeGuard([] { useStandInHardware(false); });
        useStandInHardware(true);
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::publishFailed);
        QSignalSpy ready(&engine, &SfuMediaEngine::shareTrackReady);
        engine.start();
        engine.failNextShareBackupForTest();
        engine.publishVideo(QStringLiteral("first"), true, 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(SfuMediaEngine::shareHardwareEncoderFailedForTest());
        QVERIFY(!engine.hasPublishedBinForTest(QStringLiteral("first")));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("first"));
        QCOMPARE(ready.count(), 0);

        engine.publishVideo(QStringLiteral("second"), true, 0);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(1).toString(), QString());
        QVERIFY2(engine.shareEncoderInUse().startsWith(QLatin1String("vp8enc")),
                 qPrintable(engine.shareEncoderInUse()));
        engine.stop();
    }

    void aPublishRetiredBeforeItsAnswerStopsRepeatingAndNegotiationGoesOn_data()
    {
        QTest::addColumn<bool>("afterOffer");
        QTest::newRow("before create-offer") << false;
        QTest::newRow("after create-offer, before the answer") << true;
    }
    void aPublishRetiredBeforeItsAnswerStopsRepeatingAndNegotiationGoesOn()
    {
        QFETCH(bool, afterOffer);
        SfuMediaEngine sender, receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        const QString retired = QStringLiteral("retired-early");
        QStringList offers;
        QString retireOnOffer;
        connect(&sender, &SfuMediaEngine::localDescription, this,
                [&](int target, const QString &kind, const QString &sdp) {
                    if (target != int(SfuMediaEngine::Target::Publisher)
                        || kind != QLatin1String("offer"))
                        return;
                    offers << sdp;
                    if (!retireOnOffer.isEmpty() && sdp.contains(retireOnOffer)) {
                        const QString cid = std::exchange(retireOnOffer, QString());
                        sender.unpublish(cid);
                    }
                });
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("mic"));
        QTRY_COMPARE_WITH_TIMEOUT(offers.size(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!sender.m_publisher.answeredOffer.isEmpty(), 10000);

        int settled = offers.size();
        if (afterOffer) {
            retireOnOffer = retired;
            sender.publishVideo(retired, true, 0);
            QTRY_VERIFY_WITH_TIMEOUT(retireOnOffer.isEmpty(), 10000);
        } else {
            sender.publishVideo(retired, true, 0);
            sender.unpublish(retired);
        }
        QTest::qWait(3000);
        qInfo() << "offers after the retire:" << offers.size() - settled
                << "dropped:" << sender.offersDroppedForTest();
        QVERIFY2(offers.size() - settled <= 3,
                 qPrintable(QString::number(offers.size() - settled)));
        QVERIFY(!sender.m_publisher.offerPending);

        settled = offers.size();
        const QString cid = QStringLiteral("after-the-drop");
        sender.publishVideo(cid, false, -1);
        QTRY_VERIFY_WITH_TIMEOUT(offers.size() > settled, 10000);
        QVERIFY2(offers.last().contains(cid), qPrintable(offers.last()));
        QTRY_VERIFY_WITH_TIMEOUT(
            sender.m_publisher.answeredOffer.contains(cid), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto &row : receiver.streamInfoStats()) {
                    if (row.toMap().value("streamId").toString() == cid)
                        return row.toMap().value("width").toInt() > 0;
                }
                return false;
            }(), 15000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.stop();
        receiver.stop();
    }

    void aRequestDuringADroppedOfferIsNotLost()
    {
        SfuMediaEngine sender, receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("mic"));
        QTRY_VERIFY_WITH_TIMEOUT(!sender.m_publisher.answeredOffer.isEmpty(), 10000);
        for (int i = 0; i < 5 && sender.offersDroppedForTest() == 0; ++i) {
            sender.renegotiatePublisher();
            QTest::qWait(300);
            QTRY_VERIFY_WITH_TIMEOUT(!sender.m_publisher.offerPending, 5000);
        }
        const int dropped = sender.offersDroppedForTest();
        QVERIFY(dropped > 0);
        sender.renegotiatePublisher();
        QElapsedTimer waited;
        waited.start();
        while (!sender.m_publisher.offerPending && waited.elapsed() < 5000)
            QCoreApplication::processEvents();
        QVERIFY(sender.m_publisher.offerPending);
        sender.m_publisher.negotiationDirty = true;
        QTRY_COMPARE_WITH_TIMEOUT(sender.offersDroppedForTest(), dropped + 2, 5000);
        QVERIFY(!sender.m_publisher.offerPending);
        QVERIFY(!sender.m_publisher.negotiationDirty);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.stop();
        receiver.stop();
    }

    void anOfferIdentityIgnoresOnlyWhatChangesWhenNothingDoes()
    {
        const QString offer = QStringLiteral(
            "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\n"
            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=sendonly\r\n"
            "a=candidate:1 1 udp 1 10.0.0.1 1 typ host\r\n");
        const auto identity = [](const QString &sdp) {
            return SfuMediaEngine::offerIdentity(sdp);
        };
        QString later = offer;
        later.replace(QStringLiteral("o=- 1 1"), QStringLiteral("o=- 1 2"));
        later.replace(QStringLiteral("a=candidate:1 1 udp 1 10.0.0.1 1 typ host\r\n"),
                      QStringLiteral("a=candidate:2 1 udp 2 10.0.0.2 2 typ host\r\n"
                                     "a=end-of-candidates\r\n"));
        QCOMPARE(identity(later), identity(offer));

        QString direction = offer;
        direction.replace(QStringLiteral("a=sendonly"), QStringLiteral("a=inactive"));
        QVERIFY(identity(direction) != identity(offer));

        const QString grown = offer
            + QStringLiteral("m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=sendonly\r\n");
        QVERIFY(identity(grown) != identity(offer));
    }

    void anErrorOnTheBackupEndsTheShareOnceAndLatchesSoftware()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr)
            || !SfuMediaEngine::firstMissingElement(
                    {QByteArrayLiteral("openh264enc"),
                     QByteArrayLiteral("h264parse")}).isEmpty())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        const auto restore = qScopeGuard([] { useStandInHardware(false); });
        useStandInHardware(true);
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        const QString cid = QStringLiteral("erring");
        engine.publishVideo(cid, true, 0);
        GstElement *backup = engine.publishedBinForTest(cid + QStringLiteral("-vp8"));
        QVERIFY(backup);
        QVERIFY(!SfuMediaEngine::shareHardwareEncoderFailedForTest());
        GstElement *encoder = gst_bin_get_by_name(GST_BIN(backup), "videoenc");
        QVERIFY(encoder);
        GST_ELEMENT_ERROR(encoder, LIBRARY, ENCODE, ("test"), ("test"));
        GST_ELEMENT_ERROR(encoder, LIBRARY, ENCODE, ("test"), ("test"));
        gst_object_unref(encoder);
        QTRY_COMPARE(failed.count(), 1);
        QTest::qWait(300);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), cid);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("screen_share_failed"));
        QVERIFY(SfuMediaEngine::shareHardwareEncoderFailedForTest());
        engine.stop();
    }

    void anOfferNamesEveryVideoSection()
    {
        const QString offer = QStringLiteral(
            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
            "a=ssrc:10 msid:mic webrtctransceiver0\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=rtpmap:96 VP8/90000\r\n"
            "a=ssrc:11 msid:camera webrtctransceiver1\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=rtpmap:96 VP8/90000\r\n"
            "a=ssrc:12 msid:share webrtctransceiver2\r\n");
        const QString sent = SfuMediaEngine::offerForSfu(offer, true);
        QVERIFY2(sent.contains(QLatin1String("a=ssrc:11 msid:camera camera\r\n")),
                 qPrintable(sent));
        QVERIFY(sent.contains(QLatin1String("a=ssrc:12 msid:share share\r\n")));
        QVERIFY(sent.contains(QLatin1String("a=ssrc:10 msid:mic webrtctransceiver0\r\n")));
    }

    void anOfferNamesBothCodecsOfABackedUpShare()
    {
        const QString offer = QStringLiteral(
            "v=0\r\n"
            "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
            "a=ssrc:10 msid:mic webrtctransceiver0\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
            "a=rtpmap:102 H264/90000\r\n"
            "a=ssrc:11 msid:share webrtctransceiver1\r\n"
            "a=msid:share webrtctransceiver1\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
            "a=rtpmap:96 VP8/90000\r\n"
            "a=ssrc:22 msid:share-vp8 webrtctransceiver2\r\n");
        const QString sent = SfuMediaEngine::offerForSfu(offer, true);
        const QStringList sections = sent.split(QStringLiteral("m="));
        QCOMPARE(sections.size(), 4);
        QVERIFY(sections[1].contains(QLatin1String("msid:mic webrtctransceiver0")));
        QVERIFY(!sections[1].contains(QLatin1String("VP8")));
        QVERIFY2(sections[2].startsWith(QLatin1String(
                     "video 9 UDP/TLS/RTP/SAVPF 102 96\r\n")), qPrintable(sections[2]));
        QVERIFY(sections[2].contains(QLatin1String("a=rtpmap:96 VP8/90000\r\n")));
        QVERIFY(sections[2].contains(QLatin1String("a=ssrc:11 msid:share share\r\n")));
        QVERIFY(sections[2].contains(QLatin1String("a=msid:share share\r\n")));
        QVERIFY2(sections[3].startsWith(QLatin1String(
                     "video 9 UDP/TLS/RTP/SAVPF 96 102\r\n")), qPrintable(sections[3]));
        QVERIFY(sections[3].contains(QLatin1String("a=rtpmap:102 H264/90000\r\n")));
        QVERIFY(sections[3].contains(QLatin1String("a=ssrc:22 msid:share-vp8 share-vp8\r\n")));
        QVERIFY(sent.endsWith(QLatin1String("\r\n")));
        QCOMPARE(SfuMediaEngine::offerForSfu(offer, false), offer);
    }

    void shareBackupIsEncryptedBoundedAndRetiredWithItsPrimary()
    {
        SfuMediaEngine sender, receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        sender.start();
        receiver.start();
        sender.setEncryptionRequired(true);
        receiver.setEncryptionRequired(true);
        const QByteArray key(32, 'b');
        sender.setOutboundKey(0, key);
        const QString cid = QStringLiteral("backup-primary");
        sender.setVideoSourceForTest(QStringLiteral(
            "videotestsrc is-live=true ! video/x-raw,width=1280,height=720 ! identity"));
        sender.publishVideo(cid, false, -1);
        QVERIFY(sender.publishShareBackup(cid, sender.m_publishedBins.value(cid), false));
        sender.renegotiatePublisher();
        receiver.setInboundKey(cid, 0, key);
        const QString backup = cid + QStringLiteral("-vp8");
        auto received = [&](const QString &stream) {
            for (const auto &row : receiver.streamInfoStats()) {
                const auto map = row.toMap();
                if (map.value("streamId").toString() == stream)
                    return map;
            }
            return QVariantMap();
        };
        auto receivedBackup = [&] { return received(backup); };
        QTRY_VERIFY_WITH_TIMEOUT(receivedBackup().value("dropped").toULongLong() > 0, 15000);
        QCOMPARE(receivedBackup().value("decrypted").toULongLong(), 0ULL);
        receiver.setInboundKey(backup, 0, key);
        QTRY_VERIFY_WITH_TIMEOUT(receivedBackup().value("width").toInt() > 0, 15000);
        QVERIFY(receivedBackup().value("decrypted").toULongLong() > 0);
        QVERIFY(receivedBackup().value("width").toInt() <= 640);
        QVERIFY(receivedBackup().value("height").toInt() <= 360);
        QTRY_COMPARE_WITH_TIMEOUT(received(cid).value("width").toInt(), 1280, 15000);
        QCOMPARE(sender.streamInfoStats().size(), 2);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        sender.unpublish(cid);
        QVERIFY(sender.streamInfoStats().isEmpty());
        QVERIFY(!sender.m_shareBackups.contains(cid));
        sender.stop();
        receiver.stop();
    }

    void aShareFallsBackToSoftwareWhenNoHardwareEncoderRuns()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        SfuMediaEngine engine;
        qunsetenv("LIGHTNING_SHARE_ENCODER");
        QVERIFY(!SfuMediaEngine::shareHardwareEncodeRequested());
        const SfuMediaEngine::ShareEncoderCandidate missing{
            "lightningnosuchencoder", "bitrate=%1 gop=%2",
            "constrained-baseline", true};
        const SfuMediaEngine::ShareEncoderCandidate broken{
            "vp8enc", "target-bitrate=%1 keyframe-max-dist=%2",
            "constrained-baseline", false};
        QVERIFY(!SfuMediaEngine::firstWorkingShareEncoder({missing, broken},
                                                          false));
        if (!SfuMediaEngine::firstMissingElement(
                {QByteArrayLiteral("openh264enc"),
                 QByteArrayLiteral("h264parse")}).isEmpty())
            QSKIP("no software H.264 encoder to stand in for the hardware");
        const SfuMediaEngine::ShareEncoderCandidate standIn{
            "openh264enc", "bitrate=%1 gop-size=%2",
            "constrained-baseline", false};
        const auto chosen = SfuMediaEngine::firstWorkingShareEncoder(
            {missing, broken, standIn}, false);
        QVERIFY(chosen);
        QCOMPARE(QLatin1String(chosen->candidate.factory),
                 QLatin1String("openh264enc"));
        QVERIFY(!chosen->zeroCopy);
        const QString description = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("videotestsrc is-live=true"),
            SfuMediaEngine::videoRateStage(true),
            SfuMediaEngine::shareLimitsCaps(1080, 30),
            SfuMediaEngine::shareEncoderStage(standIn, 1080, 30, 0), QString(),
            1234u, SfuMediaEngine::shareScaleStage(1080, false),
            SfuMediaEngine::captureEntryFilter(false),
            SfuMediaEngine::h264CodecChain(standIn, false));
        QVERIFY2(description.contains(QLatin1String("encoding-name=H264")),
                 qPrintable(description));
        QVERIFY(description.contains(QLatin1String("pt=102")));
        QVERIFY(description.contains(QLatin1String("payload=102")));
        GError *error = nullptr;
        GstElement *bin = gst_parse_bin_from_description(
            description.toUtf8().constData(), TRUE, &error);
        QVERIFY2(bin && !error, error ? error->message : "no bin");
        gst_object_unref(bin);
    }

    void aSecondGpusVaEncodersFollowTheFirstOnes()
    {
        const QList<QByteArray> sorted = SfuMediaEngine::sortedVaEncoders(
            {"varenderD130h264enc", "varenderD129h264enc", "varenderD1000h264enc",
             "varenderD129h264lpenc"});
        QCOMPARE(sorted, (QList<QByteArray>{"varenderD129h264lpenc",
                                             "varenderD129h264enc",
                                             "varenderD130h264enc",
                                             "varenderD1000h264enc"}));
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        QStringList ladder;
        for (const auto &candidate : SfuMediaEngine::shareEncoderCandidates())
            ladder << QString::fromLatin1(candidate.factory);
        for (const QByteArray &extra : SfuMediaEngine::extraVaH264Encoders())
            QVERIFY2(ladder.indexOf(QString::fromLatin1(extra))
                         > ladder.indexOf(QStringLiteral("vah264enc")),
                     qPrintable(ladder.join(QLatin1Char(' '))));
#endif
    }

    void aZeroCopyShareRunsTheDescriptionProductionBuilds()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        SfuMediaEngine engine;
        const auto candidates = SfuMediaEngine::shareEncoderCandidates();
        const auto chosen = SfuMediaEngine::firstWorkingShareEncoder(
            candidates, SfuMediaEngine::gpuShareChainUsable());
        if (!chosen || !chosen->zeroCopy)
            QSKIP("no encoder here takes GL memory");
        const QString description = SfuMediaEngine::videoPipelineDescription(
            QStringLiteral("gltestsrc is-live=true pattern=snow"),
            SfuMediaEngine::videoRateStage(true),
            SfuMediaEngine::shareLimitsCaps(1080, 30, true),
            SfuMediaEngine::shareEncoderStage(chosen->candidate, 1080, 30, 0),
            SfuMediaEngine::selfViewBranch(true), 1234u,
            SfuMediaEngine::shareScaleStage(1080, true, true),
            QStringLiteral("capsfilter caps=\"video/x-raw(memory:GLMemory),"
                           "format=RGBA,width=3840,height=2160,"
                           "framerate=30/1,texture-target=2D,"
                           "pixel-aspect-ratio=(fraction)1/1\""),
            SfuMediaEngine::h264CodecChain(chosen->candidate, true));
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(
            (description + QStringLiteral(" ! fakesink name=rtp")).toUtf8()
                .constData(),
            &error);
        QVERIFY2(pipeline && !error, error ? error->message : "no pipeline");
        std::atomic<int> packets{0};
        std::atomic<int> previews{0};
        GstElement *rtp = gst_bin_get_by_name(GST_BIN(pipeline), "rtp");
        GstPad *pad = gst_element_get_static_pad(rtp, "sink");
        const auto count = [](GstPad *, GstPadProbeInfo *, gpointer data) {
            static_cast<std::atomic<int> *>(data)->fetch_add(1);
            return GST_PAD_PROBE_OK;
        };
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count, &packets,
                          nullptr);
        GstElement *self = gst_bin_get_by_name(GST_BIN(pipeline), "selfvidsink");
        GstPad *selfPad = gst_element_get_static_pad(self, "sink");
        gst_pad_add_probe(selfPad, GST_PAD_PROBE_TYPE_BUFFER, count, &previews,
                          nullptr);
        GstBus *bus = gst_element_get_bus(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        QTest::qWaitFor(
            [&] {
                return packets.load() > 20 && previews.load() > 5;
            },
            8000);
        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        QString failure;
        if (message) {
            GError *why = nullptr;
            gst_message_parse_error(message, &why, nullptr);
            failure = QString::fromUtf8(why ? why->message : "?");
            if (why)
                g_error_free(why);
            gst_message_unref(message);
        }
        const int sent = packets.load();
        const int shown = previews.load();
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pad);
        gst_object_unref(selfPad);
        gst_object_unref(rtp);
        gst_object_unref(self);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(sent > 20 && shown > 5,
                 qPrintable(QStringLiteral("%1 packets, %2 previews")
                                .arg(sent).arg(shown)));
    }

    void aFailedHardwareEncoderEndsOnlyTheShareItBelongsTo()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        QSignalSpy failed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        const QString cid = QStringLiteral("cid-share");
        engine.publishVideo(cid, true, -1);
        QCOMPARE(engine.shareEncoderInUse().section(QLatin1Char(' '), 0, 0),
                 QStringLiteral("vp8enc"));
        const auto row = engine.streamInfoStats().first().toMap();
        QVERIFY(row.value("screen").toBool());
        QCOMPARE(row.value("encoder").toString(), engine.shareEncoderInUse());
        QCOMPARE(row.value("codec").toString(), QStringLiteral("VP8"));
        engine.handleShareEncoderFailed(QStringLiteral("cid-old-share"));
        QCOMPARE(failed.count(), 0);
        engine.handleShareEncoderFailed(cid);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), cid);
        engine.handleShareEncoderFailed(cid);
        QCOMPARE(failed.count(), 1);
    }

    void aChosenCameraIsBoundThroughTheOneResolverEveryLaneUses()
    {
        if (!gst_init_check(nullptr, nullptr, nullptr))
            QSKIP("no GStreamer");
        const auto refused = SfuMediaEngine::resolveCameraBinding(
            QStringLiteral("notacamerasrc device=x"),
            {QStringLiteral("/dev/video7"), QStringLiteral("Cam")});
        QVERIFY(refused.binding.refused);
        QVERIFY(refused.refusal == QLatin1String("camera_unavailable")
                || refused.refusal == QLatin1String("camera_list_unavailable"));
        const auto none = SfuMediaEngine::resolveCameraBinding(
            QStringLiteral("v4l2src"), {});
        QVERIFY(none.binding.isEmpty());
        QVERIFY(!none.binding.refused);
        QVERIFY(none.refusal.isEmpty());
    }

    void aGpuShareStaysInGlMemoryOnlyForAnEncoderThatTakesIt()
    {
        QVERIFY(!SfuMediaEngine::shareScaleStage(1080, true, true)
                     .contains(QLatin1String("gldownload")));
        QVERIFY(SfuMediaEngine::shareScaleStage(1080, true)
                    .contains(QLatin1String("gldownload")));
        QVERIFY(SfuMediaEngine::shareLimitsCaps(1080, 30, true)
                    .contains(QLatin1String("(memory:GLMemory)")));
        QVERIFY(SfuMediaEngine::selfViewBranch(true)
                    .contains(QLatin1String("gldownload")));
    }

    void shareAudioIsOfferedOnlyWhenSomethingCanActuallyCaptureIt()
    {
        const bool available = SfuMediaEngine::shareAudioAvailable();

        bool anyPresent = false;
#if defined(Q_OS_WIN)
        if (GstElement *probe = gst_element_factory_make("wasapi2src", nullptr)) {
            anyPresent = g_object_class_find_property(
                             G_OBJECT_GET_CLASS(probe), "loopback-target-pid")
                && g_object_class_find_property(
                             G_OBJECT_GET_CLASS(probe), "loopback-mode");
            gst_object_unref(probe);
        }
#endif
        const bool perApplication =
            lightning::shareaudio::perApplicationCaptureAvailable();
        const bool haveProvider =
            gst_device_provider_factory_find("pipewiredeviceprovider")
            != nullptr;
        bool haveMixer = false;
        if (GstElementFactory *mix = gst_element_factory_find("audiomixer")) {
            gst_object_unref(mix);
            haveMixer = true;
        }
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
                     "unable to capture other applications while leaving us out "
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
        if (anyPresent || perApplication)
            QVERIFY(available);

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

        QVERIFY2(source.contains(QStringLiteral(
                     "static_cast<gsize>(self->outWidth) * self->outHeight")),
                 "the frame size is no longer computed from the negotiated "
                 "output size");

        QVERIFY2(!source.contains(QStringLiteral("GetClientRect(")),
                 "the capture is measuring the client rect again while "
                 "PrintWindow draws the whole window");

        QVERIFY2(source.contains(QStringLiteral("wincap::fitInto(")),
                 "the element no longer uses the shared fitting rule");
    }
    void theCameraJpegEntryDecodesExplicitlyAndKeepsThePixelAspectPin()
    {
        const QString entry = SfuMediaEngine::cameraJpegEntry();

        QVERIFY2(entry.contains(QStringLiteral("image/jpeg")),
                 "the camera entry does not accept MJPG, so a camera still "
                 "cannot negotiate one");

        QVERIFY2(entry.contains(QStringLiteral("jpegdec")),
                 "the MJPG entry does not decode explicitly");
        QVERIFY2(!entry.contains(QStringLiteral("decodebin")),
                 "decodebin is refuted for this chain — delayed linking kills "
                 "the capture; use jpegdec, which links at parse time");

        QVERIFY2(entry.contains(
                     QStringLiteral("pixel-aspect-ratio=(fraction)1/1")),
                 "the MJPG entry dropped the pixel-aspect-ratio pin that the "
                 "raw entry applies");

        QVERIFY(entry.contains(QStringLiteral("video/x-raw")));
    }

    void aYuy2OnlyCameraSelectsAFullyFixedRawEntry()
    {
        GError *error = nullptr;
        GstElement *bin = gst_parse_bin_from_description(
            "videotestsrc name=capsrc ! capsfilter "
            "caps=video/x-raw,format=YUY2,width=1280,height=720,"
            "framerate=30/1", TRUE, &error);
        QVERIFY2(bin, error ? error->message : "camera probe bin failed");
        g_clear_error(&error);
        QString ioMode;
        const QString probedEntry = SfuMediaEngine::captureCameraEntry(
            bin, true, &ioMode);
        QVERIFY(probedEntry.contains(QStringLiteral("video/x-raw")));
        QVERIFY(!probedEntry.contains(QStringLiteral("jpegdec")));
        gst_object_unref(bin);

        GstCaps *offered = gst_caps_from_string(
            "video/x-raw,format=(string){YUY2,NV12},width=(int)1280,"
            "height=(int)720,framerate=(fraction){15/1,30/1},"
            "pixel-aspect-ratio=(fraction)1/1");
        QVERIFY(offered);
        GstCaps *chosen = SfuMediaEngine::preferredCameraCaps(offered, true);
        QVERIFY(chosen);
        QVERIFY(!SfuMediaEngine::capsOfferJpeg(chosen));
        QVERIFY(gst_caps_is_fixed(chosen));
        const QString entry = SfuMediaEngine::directCameraRawEntry(chosen);
        QVERIFY(entry.contains(QStringLiteral("video/x-raw")));
        QVERIFY(entry.contains(QStringLiteral("format=(string)")));
        QVERIFY(!entry.contains(QStringLiteral("{YUY2,NV12}")));
        QVERIFY(!entry.contains(QStringLiteral("jpegdec")));
        gst_caps_unref(chosen);
        gst_caps_unref(offered);
    }

    void publishShareAudioWithNoExcludingCaptureFailsAndAddsNoBin()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.start();
        engine.setShareAudioCaptureForTest({});
        QSignalSpy failed(&engine, &SfuMediaEngine::failed);

        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-sys"), system);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.takeFirst().at(0).toString(),
                 QStringLiteral("share_audio_unavailable"));
        QVERIFY(!engine.hasPublishedBinForTest(QStringLiteral("cid-sys")));

        lightning::shareaudio::Selection apps;
        apps.mode = Mode::Apps;
        apps.keys = { QStringLiteral("firefox") };
        engine.publishShareAudio(QStringLiteral("cid-apps"), apps);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.takeFirst().at(0).toString(),
                 QStringLiteral("share_audio_apps_unavailable"));
        QVERIFY(!engine.hasPublishedBinForTest(QStringLiteral("cid-apps")));
    }

    void windowsShareAudioPrefersExcludingOurOwnProcess()
    {
        using namespace lightning::shareaudio;
        const auto source = systemCaptureFor(Platform::Windows, false, true, 4242);
        QVERIFY(source.excludesUs);
        QVERIFY(source.description.contains(
            QStringLiteral("loopback-mode=exclude-process-tree")));
        QVERIFY(source.description.contains(QStringLiteral("loopback-target-pid=4242")));
        QVERIFY(source.description.contains(QStringLiteral("name=sharesrc")));
        QVERIFY(systemCaptureFor(Platform::Windows, false, false, 4242).description.isEmpty());
        QVERIFY(systemCaptureFor(Platform::Windows, false, true, 0).description.isEmpty());
#if defined(Q_OS_WIN)
        if (systemExclusion().possible) {
            const QString description = encodedTrackDescription(source.description, 1234);
            GError *error = nullptr;
            GstElement *bin = gst_parse_bin_from_description(
                description.toUtf8().constData(), TRUE, &error);
            const bool parsed = bin && !error;
            if (error)
                g_error_free(error);
            if (bin)
                gst_object_unref(bin);
            QVERIFY(parsed);
        }
#endif
    }

    void aShareAudioBranchParsesStandaloneTheWayTheDynamicPathBuildsIt()
    {
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

    void theWholeShareAudioTrackParsesTheWayPublishShareAudioComposesIt()
    {
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

        const QString single = lightning::shareaudio::encodedTrackDescription(
            QStringLiteral("audiotestsrc name=sharesrc is-live=true "
                           "wave=silence"),
            0x1234u);
        QVERIFY2(parseFailure(single).isEmpty(),
                 qPrintable(parseFailure(single)));

        const auto windows = lightning::shareaudio::systemCaptureFor(
            lightning::shareaudio::Platform::Windows, false, true, 4242);
        QVERIFY(windows.description.contains(QStringLiteral("name=sharesrc")));
        QVERIFY(windows.description.contains(
            QStringLiteral("loopback-mode=exclude-process-tree")));
        const auto unsupported = lightning::shareaudio::systemCaptureFor(
            lightning::shareaudio::Platform::Linux, false, false, 4242);
        QVERIFY(unsupported.description.isEmpty());
    }

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
                0x1234u,true);
            const QString message = parseFailure(whole);
            QVERIFY2(message.isEmpty(),
                     qPrintable(QStringLiteral(
                         "the chosen-applications track (on-disconnect=%1) "
                         "does not parse: %2").arg(retire).arg(message)));
            QVERIFY(whole.contains(QStringLiteral("shareappvol1")));
        }

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

    void aShareAudioSelectionWithoutARunningShareIsOnlyRecorded()
    {
        SfuMediaEngine engine;
        lightning::shareaudio::Selection apps;
        apps.mode = lightning::shareaudio::Mode::Apps;
        apps.keys = { QStringLiteral("firefox") };
        QVERIFY(engine.applyShareAudioSelection(apps));
        lightning::shareaudio::Selection none;
        none.mode = lightning::shareaudio::Mode::Apps;
        engine.publishShareAudio(QStringLiteral("cid-none"), none);
        QVERIFY(!engine.hasPublishedBinForTest(QStringLiteral("cid-none")));
    }

    void aPerApplicationShareFollowsTheChoiceAndTheStreams()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("11"), QStringLiteral("a")),
                          scriptedStream(QStringLiteral("12"), QStringLiteral("b")) };
        installListing(engine, listing,false);
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
        QVERIFY(reports.last().at(1).toBool());

        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        QVERIFY(engine.applyShareAudioSelection(system));
        QTest::qWait(200);
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);
        QVERIFY(!engine.shareAudioBranchMutedForTest(QStringLiteral("11")));

        lightning::shareaudio::Selection onlyB;
        onlyB.mode = Mode::Apps;
        onlyB.keys = { QStringLiteral("b") };
        QVERIFY(engine.applyShareAudioSelection(onlyB));
        QVERIFY(engine.shareAudioBranchMutedForTest(QStringLiteral("11")));
        QVERIFY(!engine.shareAudioBranchMutedForTest(QStringLiteral("12")));
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);

        listing->live = { scriptedStream(QStringLiteral("12"), QStringLiteral("b")) };
        engine.rescanShareAudioSourcesForTest();
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("11")) >= 0);
        QTest::qWait(300);
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(QStringLiteral("11")), -2);
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 1, 3000);
        QCOMPARE(engine.shareAudioBranchCountForTest(), 1);
        QTest::qWait(200);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-share-audio")));
        engine.stop();
    }

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
        installListing(engine, listing,true);
        engine.start();
        lightning::shareaudio::Selection apps;
        apps.mode = Mode::Apps;
        apps.keys = { QStringLiteral("chrome.exe") };
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), apps);
        QTest::qWait(300);
        const int first = engine.shareAudioBranchIndexForTest(browser.id());
        QVERIFY(first >= 0);

        listing->live = {};
        engine.rescanShareAudioSourcesForTest();
        QTest::qWait(200);
        engine.rescanShareAudioSourcesForTest();
        QVERIFY2(engine.shareAudioBranchIndexForTest(browser.id()) == first,
                 "an expired session cost its process the capture; it would "
                 "never be captured again");
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);

        listing->live = { browser };
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(browser.id()), first);
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);

        listing->live = {};
        listing->running = {};
        engine.rescanShareAudioSourcesForTest();
        QTest::qWait(300);
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(browser.id()), -2);
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 0, 3000);

        listing->live = { browser };
        listing->running = { browser.id() };
        engine.rescanShareAudioSourcesForTest();
        const int again = engine.shareAudioBranchIndexForTest(browser.id());
        QVERIFY2(again >= 0 && again != first,
                 "a returning application was never captured again");
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 1);
        engine.stop();
    }

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
        installListing(engine, listing,false);
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

        listing->live.removeFirst();
        engine.rescanShareAudioSourcesForTest();
        QTest::qWait(300);
        engine.rescanShareAudioSourcesForTest();
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 2, 3000);
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("33")) >= 0);
        QVERIFY(!reports.last().at(5).toBool());
        engine.stop();
    }

    void aBranchThatEndsByItselfIsReplacedEvenIfItsIdComesBack()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("37"), QStringLiteral("toneb")) };
        installListing(engine, listing,false);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        const int first = engine.shareAudioBranchIndexForTest(QStringLiteral("37"));
        QVERIFY(first >= 0);

        engine.sendShareAudioEosForTest(QStringLiteral("37"));
        QTRY_VERIFY_WITH_TIMEOUT(
            engine.shareAudioEosSeenForTest(QStringLiteral("37")), 3000);
        engine.rescanShareAudioSourcesForTest();
        engine.rescanShareAudioSourcesForTest();
        const int again = engine.shareAudioBranchIndexForTest(QStringLiteral("37"));
        QVERIFY2(again >= 0 && again != first,
                 "an ended branch was kept for a live stream: silence");
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 1, 3000);
        engine.stop();
    }

    void aBranchWhoseDaemonHungUpIsReplaced()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("37"), QStringLiteral("toneb")) };
        installListing(engine, listing,false);
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

        engine.rescanShareAudioSourcesForTest();
        engine.rescanShareAudioSourcesForTest();
        QCOMPARE(engine.shareAudioBranchIndexForTest(QStringLiteral("37")), first);

        ::close(pair[1]);
        engine.setShareAudioSuppressRetireEosForTest(true);
        engine.rescanShareAudioSourcesForTest();
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

    void aDaemonRestartUnderAShareAsksForAFreshShareAudioTrack()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = {
            scriptedStream(QStringLiteral("37"), QStringLiteral("tonea")),
            scriptedStream(QStringLiteral("38"), QStringLiteral("toneb")) };
        installListing(engine, listing,false);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("37")) >= 0);
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("38")) >= 0);
        QSignalSpy wanted(&engine, &SfuMediaEngine::shareAudioRebuildWanted);
        engine.setShareAudioSuppressRetireEosForTest(true);

        const auto watch = [&engine](const QString &id, int pair[2]) {
            QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
                                  pair),
                     0);
            engine.setShareAudioConnectionForTest(id, pair[0]);
        };
        int a[2] = { -1, -1 };
        int b[2] = { -1, -1 };
        watch(QStringLiteral("37"), a);
        watch(QStringLiteral("38"), b);

        ::close(a[1]);
        engine.rescanShareAudioSourcesForTest();
        QCoreApplication::processEvents();
        QCOMPARE(wanted.count(), 0);

        ::close(b[1]);
        int c[2] = { -1, -1 };
        const int recaptured =
            engine.shareAudioBranchIndexForTest(QStringLiteral("37"));
        QVERIFY(recaptured >= 0);
        watch(QStringLiteral("37"), c);
        ::close(c[1]);
        engine.rescanShareAudioSourcesForTest();
        QVERIFY2(wanted.isEmpty(),
                 "asked for a new track from inside the scan that still "
                 "uses the old one");
        QTRY_COMPARE_WITH_TIMEOUT(wanted.count(), 1, 2000);
        QCOMPARE(wanted.at(0).at(0).toString(),
                 QStringLiteral("cid-share-audio"));

        int d[2] = { -1, -1 };
        int e[2] = { -1, -1 };
        const QString again37 = QStringLiteral("37");
        const QString again38 = QStringLiteral("38");
        if (engine.shareAudioBranchIndexForTest(again37) >= 0
            && engine.shareAudioBranchIndexForTest(again38) >= 0) {
            watch(again37, d);
            watch(again38, e);
            ::close(d[1]);
            ::close(e[1]);
            engine.rescanShareAudioSourcesForTest();
            QTest::qWait(100);
            QCOMPARE(wanted.count(), 1);
        }
        engine.stop();
        for (int fd : { a[0], b[0], c[0], d[0], e[0] }) {
            if (fd >= 0)
                ::close(fd);
        }
#else
        QSKIP("PipeWire is Linux-only");
#endif
    }

    void aBranchStillStartingDoesNotHideADaemonRestart()
    {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = {
            scriptedStream(QStringLiteral("37"), QStringLiteral("tonea")) };
        installListing(engine, listing,false);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        QVERIFY(engine.shareAudioBranchIndexForTest(QStringLiteral("37")) >= 0);
        QVERIFY(!engine.shareAudioBranchStartingForTest(QStringLiteral("37")));

        engine.setShareAudioStartDelayForTest(3000);
        listing->live.append(
            scriptedStream(QStringLiteral("38"), QStringLiteral("toneb")));
        engine.rescanShareAudioSourcesForTest();
        QVERIFY(engine.shareAudioBranchStartingForTest(QStringLiteral("38")));

        QSignalSpy wanted(&engine, &SfuMediaEngine::shareAudioRebuildWanted);
        engine.setShareAudioSuppressRetireEosForTest(true);
        int a[2] = { -1, -1 };
        int b[2] = { -1, -1 };
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, a), 0);
        QCOMPARE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, b), 0);
        engine.setShareAudioConnectionForTest(QStringLiteral("37"), a[0]);
        engine.setShareAudioConnectionForTest(QStringLiteral("38"), b[0]);
        ::close(a[1]);
        engine.rescanShareAudioSourcesForTest();
        QTRY_COMPARE_WITH_TIMEOUT(wanted.count(), 1, 2000);
        engine.stop();
        QTest::qWait(3500);
        ::close(a[0]);
        ::close(b[0]);
        ::close(b[1]);
#else
        QSKIP("PipeWire is Linux-only");
#endif
    }

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

        QThread::msleep(1500);
        QVERIFY2(router.pendingFramesForTest() <= 1,
                 "frames piled up for a stalled GUI thread");
        frames.clear();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const int burst = frames.count();
        QVERIFY2(burst <= 2,
                 qPrintable(QStringLiteral("%1 frames delivered at once after "
                                           "a 1.5 s stall; each was held in "
                                           "memory meanwhile")
                                .arg(burst)));
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= burst + 3, 5000);
        engine.stop();
        engine.setVideoRouter(nullptr);
    }

    void aSlowBranchStartDoesNotBlockTheGuiThread()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("51"), QStringLiteral("toneb")) };
        installListing(engine, listing,false);
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

    void aBranchThatWouldNotStartIsRetried()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("52"), QStringLiteral("tonea")) };
        installListing(engine, listing,false);
        engine.failShareAudioStartsForTest(1);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioFailedKeysForTest(),
                                  QStringList{ QStringLiteral("tonea") }, 5000);
        QVERIFY(engine.shareAudioCarriedForTest().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(engine.shareAudioBranchBinsForTest(), 0, 3000);
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

    void aRestartThatDeliversNoLongerCountsAgainstTheMicBudget()
    {
        QVERIFY(!SfuMediaEngine::micRestartBudgetResets(0, 5000, true));
        QVERIFY(!SfuMediaEngine::micRestartBudgetResets(3, 15000, false));
        QVERIFY(!SfuMediaEngine::micRestartBudgetResets(3, 5000, true));
        QVERIFY(SfuMediaEngine::micRestartBudgetResets(3, 10000, true));
        QVERIFY(SfuMediaEngine::micRestartBudgetResets(5, 60001, false));
    }

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

    void aRetiredBranchIsNotTakenOutUntilItsEosHasPassed()
    {
        using lightning::shareaudio::Mode;
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        auto listing = std::make_shared<ScriptedListing>();
        listing->live = { scriptedStream(QStringLiteral("41"), QStringLiteral("a")),
                          scriptedStream(QStringLiteral("42"), QStringLiteral("b")) };
        installListing(engine, listing,false);
        engine.start();
        lightning::shareaudio::Selection system;
        system.mode = Mode::System;
        engine.publishShareAudio(QStringLiteral("cid-share-audio"), system);
        QTest::qWait(300);
        QCOMPARE(engine.shareAudioBranchBinsForTest(), 2);

        engine.setShareAudioSuppressRetireEosForTest(true);
        listing->live = { scriptedStream(QStringLiteral("42"), QStringLiteral("b")) };
        engine.rescanShareAudioSourcesForTest();
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

    void anUndeliveredKeyIsInstalledButNotAdopted()
    {
        SfuMediaEngine engine;
        engine.setOutboundKey(1, QByteArray(32, 'a'));
        QCOMPARE(engine.adoptedOutboundKeyIndexForTest(), 1);

        engine.setOutboundKey(2, QByteArray(32, 'b'),false);
        QCOMPARE(engine.adoptedOutboundKeyIndexForTest(), 1);

        engine.setOutboundKey(3, QByteArray(32, 'c'),true);
        QCOMPARE(engine.adoptedOutboundKeyIndexForTest(), 3);
    }

    void aServerInjectedBurstIsDroppedApartAndNeverBadges()
    {
        SfuMediaEngine engine;
        const QString stream = QStringLiteral("PA_sif");
        const QByteArray key(16, 'e');
        engine.setEncryptionRequired(true);
        engine.setInboundKey(stream, 82, key);
        engine.setServerInjectedTrailer(roomTrailer());
        QCOMPARE(engine.serverInjectedTrailer(), roomTrailer());

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
        QCOMPARE(rig.passed(), 5);
        QVERIFY2(!log.contains("will not DECRYPT"),
                 qPrintable(log.text()));
        QVERIFY(log.contains("SERVER-INJECTED"));
        QTest::qWait(100);
        for (const QList<QVariant> &call : blocked) {
            QVERIFY2(call.at(1).toString().isEmpty(),
                     qPrintable(QStringLiteral(
                         "a mute's injected frames raised the badge: %1")
                                    .arg(call.at(1).toString())));
        }
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

        rig.finish();
        QVERIFY2(log.contains("server-injected burst")
                     && log.text().contains(QRegularExpression(
                         QStringLiteral("frames= 50\\b"))),
                 qPrintable(log.text()));
        QVERIFY2(log.text().contains(QRegularExpression(
                     QStringLiteral("decrypt histogram final.*sif= 50"))),
                 qPrintable(log.text()));
    }

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
        QVERIFY2(log.text().contains(QRegularExpression(QStringLiteral(
                     "decrypt failed detail .*reason= bad-iv-length size= 123 "
                     "ivLenByte= \\d+ keyIndexByte= 82 .*silenceShape= true "
                     "tailBase62= true sifArmed= false"))),
                 qPrintable(log.text()));
        QCOMPARE(log.count("decrypt failed detail"), 6);
    }

    void theTrailerIsBoundedAndClearedWithTheKeys()
    {
        SfuMediaEngine engine;
        engine.setServerInjectedTrailer(roomTrailer());
        QCOMPARE(engine.serverInjectedTrailer(), roomTrailer());
        engine.setServerInjectedTrailer(
            QByteArray(CallFrameCryptor::kMaxServerTrailerBytes + 1, 'A'));
        QVERIFY2(engine.serverInjectedTrailer().isEmpty(),
                 "an oversized trailer was armed (or kept the old one)");
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
        rig.push(QByteArray(60, 'z'));
        rig.push(QByteArray(60, 'y'));
        rig.push(otherIndex.encryptFrame(QByteArray("\x01") + QByteArray(30, 'q'),
                                         CallFrameCryptor::FrameKind::Audio,
                                         3, 960));
        QVERIFY(rig.drain());

        QCOMPARE(log.count("will not DECRYPT: reason= bad-iv-length"), 1);
        QVERIFY2(log.contains("will not DECRYPT: reason= no-key-for-index "
                              "keyIndex= 5"),
                 qPrintable(log.text()));
    }

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
        const int arrived = log.count("media key ARRIVED");
        QVERIFY2(arrived >= 3 && arrived <= 3 * (8 + 200 / 16),
                 qPrintable(QString::number(arrived)));
        engine.noteParticipantIdentity(QString(),
                                       QStringLiteral("@a:example.org:DEV"));
        QVERIFY2(log.contains("could NOT be bound to a sending stream"),
                 "key arrivals used up the diagnosis set");
    }

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
        engine.setInboundKey(stream, 256, key);
        QVERIFY(log.contains("REFUSED by the cryptor"));
    }

    void aTrackThatNeverDeliversDoesNotSilenceTheOthers()
    {
        struct Counter { std::atomic<int> n{0}; };
        Counter keyed;
        Counter keyless;
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
        const auto adopt = [pipeline](GstElement *bin) {
            return bin && gst_bin_add(GST_BIN(pipeline), bin);
        };
        const auto syncAndLink = [pipeline](GstElement *bin,
                                            const char *teeName) {
            gst_element_sync_state_with_parent(bin);
            if (!teeName)
                return true;
            GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), teeName);
            GstPad *src = gst_element_request_pad_simple(tee, "src_%u");
            GstPad *sinkPad = gst_element_get_static_pad(bin, "sink");
            const bool linked = gst_pad_link(src, sinkPad) == GST_PAD_LINK_OK;
            gst_object_unref(sinkPad);
            gst_object_unref(src);
            gst_object_unref(tee);
            return linked;
        };

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

        GstElement *withKey = SfuMediaEngine::buildReceiveBin(
            false, QStringLiteral("outvol_keyed"), true, &why);
        QVERIFY2(adopt(withKey), qPrintable(why));
        GstElement *keyedSink = countAtSink(withKey, &keyed);
        QVERIFY(keyedSink);
        QVERIFY(syncAndLink(withKey, "ta"));

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
        const GstClockTime latency =
            gst_base_sink_get_latency(GST_BASE_SINK(keyedSink));
        QVERIFY2(latency >= 100 * GST_MSECOND,
                 qPrintable(QStringLiteral("the receive sink was configured "
                                           "for %1 ms of latency behind a "
                                           "100 ms jitterbuffer")
                                .arg(latency / GST_MSECOND)));

        keyArrived.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(keyless.n.load() > 20, 5000);
        const int keyedNow = keyed.n.load();
        QTRY_VERIFY_WITH_TIMEOUT(keyed.n.load() > keyedNow + 10, 5000);
    }

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

        QTRY_VERIFY_WITH_TIMEOUT(sent.load() > 10, 5000);
        GstState state = GST_STATE_VOID_PENDING;
        gst_element_get_state(pipeline, &state, nullptr, 0);
        QCOMPARE(state, GST_STATE_PLAYING);
    }

    void cameraModesPreferAnAdvertisedBoundedSize()
    {
        for (const char *type : {"video/x-raw", "image/jpeg"}) {
            const QByteArray advertised = QByteArray(type)
                + ",width=3840,height=2160,framerate=60/1;" + type
                + ",width=1280,height=720,framerate=60/1;" + type
                + ",width=1920,height=1080,framerate=60/1";
            GstCaps *caps = gst_caps_from_string(advertised.constData());
            GstCaps *chosen = SfuMediaEngine::preferredCameraCaps(caps, true);
            QVERIFY(chosen);
            int width = 0, height = 0;
            gst_structure_get_int(gst_caps_get_structure(chosen, 0), "width", &width);
            gst_structure_get_int(gst_caps_get_structure(chosen, 0), "height", &height);
            QCOMPARE(width, 1920);
            QCOMPARE(height, 1080);
            QVERIFY(gst_caps_is_subset(chosen, caps));
            gst_caps_unref(chosen);
            gst_caps_unref(caps);
        }
        GstCaps *mixed = gst_caps_from_string(
            "image/jpeg,width=3840,height=2160;"
            "video/x-raw,width=1280,height=720");
        GstCaps *chosen = SfuMediaEngine::preferredCameraCaps(mixed, true);
        QVERIFY(chosen);
        QVERIFY(!SfuMediaEngine::capsOfferJpeg(chosen));
        GstCaps *jpeg1080 = gst_caps_from_string(
            "image/jpeg,width=1920,height=1080");
        QVERIFY(SfuMediaEngine::capsOfferJpeg(mixed));
        QVERIFY(!SfuMediaEngine::capsOfferJpeg(mixed, jpeg1080));
        gst_caps_unref(jpeg1080);
        gst_caps_unref(chosen);
        gst_caps_unref(mixed);
    }

    void cameraModesPreferPublishRate_data()
    {
        QTest::addColumn<QString>("modes");
        QTest::addColumn<QString>("preferred");
        QTest::addColumn<int>("alternatives");
        QTest::addColumn<bool>("reachesPublishRate");
        const auto add = [](const char *name, const char *modes,
                            const char *preferred, int alternatives, bool reaches) {
            QTest::newRow(name) << QString::fromLatin1(modes)
                               << QString::fromLatin1(preferred)
                               << alternatives << reaches;
        };
        add("mixed-5-and-30",
            "width=1920,height=1080,framerate=5/1;"
               "width=1280,height=720,framerate=30/1",
            "width=1280,height=720,framerate=30/1", 1, true);
        add("mixed-5-and-60",
            "width=1920,height=1080,framerate=5/1;"
               "width=1280,height=720,framerate=60/1",
            "width=1280,height=720,framerate=60/1", 1, true);
        add("area-breaks-rate-tie-and-keeps-alternative",
            "width=1280,height=720,framerate=60/1;"
               "width=1920,height=1080,framerate=30/1",
            "width=1920,height=1080,framerate=30/1", 2, true);
        add("fraction-list-excludes-slow-rate",
            "width=1920,height=1080,framerate=(fraction){5/1,30/1};"
               "width=1280,height=720,framerate=60/1",
            "width=1920,height=1080,framerate=30/1", 2, true);
        add("fraction-range-excludes-slow-rates",
            "width=1920,height=1080,framerate=(fraction)[5/1,60/1];"
               "width=1280,height=720,framerate=30/1",
            "width=1920,height=1080,framerate=30/1", 2, true);
        add("slow-only-prefers-motion",
            "width=1920,height=1080,framerate=5/1;"
               "width=1280,height=720,framerate=(fraction)[5/1,25/1]",
            "width=1280,height=720,framerate=25/1", 2, false);
        add("above-1080p-only",
            "width=3840,height=2160,framerate=5/1;"
               "width=2560,height=1440,framerate=30/1",
            "width=2560,height=1440,framerate=30/1", 1, true);
        add("60-only",
            "width=1920,height=1080,framerate=60/1",
            "width=1920,height=1080,framerate=60/1", 1, true);
    }
    void cameraModesPreferPublishRate()
    {
        QFETCH(QString, modes);
        QFETCH(QString, preferred);
        QFETCH(int, alternatives);
        QFETCH(bool, reachesPublishRate);
        for (const QString &type : {QStringLiteral("video/x-raw"),
                                    QStringLiteral("image/jpeg")}) {
            QString advertised = type + QLatin1Char(',') + modes;
            advertised.replace(QLatin1Char(';'), QLatin1Char(';') + type + QLatin1Char(','));
            GstCaps *caps = gst_caps_from_string(advertised.toUtf8().constData());
            QVERIFY(caps);
            GstCaps *chosen = SfuMediaEngine::preferredCameraCaps(caps, true);
            QVERIFY(chosen);
            QCOMPARE(gst_caps_get_size(chosen), guint(alternatives));
            QVERIFY(gst_caps_is_subset(chosen, caps));
            GstCaps *first = gst_caps_copy_nth(chosen, 0);
            GstCaps *expected = gst_caps_from_string(
                (type + QLatin1Char(',') + preferred).toUtf8().constData());
            QVERIFY(gst_caps_is_subset(expected, first));
            if (reachesPublishRate) {
                GstCaps *slow = gst_caps_from_string(
                    (type + QStringLiteral(",framerate=(fraction)[0/1,29999/1000]"))
                        .toUtf8().constData());
                QVERIFY(!gst_caps_can_intersect(chosen, slow));
                gst_caps_unref(slow);
            }
            gst_caps_unref(expected);
            gst_caps_unref(first);
            gst_caps_unref(chosen);
            gst_caps_unref(caps);
        }
    }

    void cameraMotionWinsOverFormatAndBounds()
    {
        for (const char *advertised : {
                 "image/jpeg,width=1920,height=1080,framerate=5/1;"
                 "video/x-raw,width=1280,height=720,framerate=30/1",
                 "image/jpeg,width=1280,height=720,framerate=5/1;"
                 "video/x-raw,width=3840,height=2160,framerate=60/1"}) {
            GstCaps *caps = gst_caps_from_string(advertised);
            GstCaps *chosen = SfuMediaEngine::preferredCameraCaps(caps, true);
            QVERIFY(chosen);
            QVERIFY(gst_caps_is_subset(chosen, caps));
            QVERIFY(!SfuMediaEngine::capsOfferJpeg(chosen));
            gst_caps_unref(chosen);
            gst_caps_unref(caps);
        }
    }

    void fixedCameraModesNegotiateAndScale_data()
    {
        QTest::addColumn<bool>("jpeg");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<bool>("probeFormats");
        QTest::addColumn<int>("captureFps");
        QTest::addColumn<QString>("otherModes");
        QTest::newRow("raw-4k-only") << false << 3840 << 2160 << true << 60 << QString();
        QTest::newRow("jpeg-4k-only") << true << 3840 << 2160 << true << 60 << QString();
        QTest::newRow("raw-1080p60") << false << 1920 << 1080 << true << 60 << QString();
        QTest::newRow("jpeg-1080p60") << true << 1920 << 1080 << true << 60 << QString();
        QTest::newRow("unprobed-raw-4k") << false << 3840 << 2160 << false << 60 << QString();
        QTest::newRow("unprobed-jpeg-4k") << true << 3840 << 2160 << false << 60 << QString();
        for (const bool jpeg : {false, true}) {
            const QByteArray prefix = jpeg ? "jpeg-" : "raw-";
            QTest::newRow((prefix + "mixed-rate").constData())
                << jpeg << 1280 << 720 << true << 30
                << QStringLiteral("width=1920,height=1080,framerate=5/1");
            QTest::newRow((prefix + "mixed-60-only").constData())
                << jpeg << 1280 << 720 << true << 60
                << QStringLiteral("width=1920,height=1080,framerate=5/1");
            QTest::newRow((prefix + "rate-list").constData())
                << jpeg << 1920 << 1080 << true << 30
                << QStringLiteral("width=1920,height=1080,framerate=(fraction){5/1,30/1};"
                                  "width=1280,height=720,framerate=30/1");
        }
    }
    void fixedCameraModesNegotiateAndScale()
    {
        lightning::rtp::registerVp8Payloader();
        QFETCH(bool, jpeg);
        if (jpeg && !SfuMediaEngine::jpegCameraChainAvailable())
            QSKIP("no JPEG camera decoder here");
        QFETCH(int, width);
        QFETCH(int, height);
        QFETCH(bool, probeFormats);
        QFETCH(int, captureFps);
        QFETCH(QString, otherModes);
        QString modes = QStringLiteral("width=%1,height=%2,framerate=%3/1")
                            .arg(width).arg(height).arg(captureFps);
        if (!otherModes.isEmpty())
            modes = otherModes + QLatin1Char(';') + modes;
        const QString rawPrefix = QStringLiteral(
            "video/x-raw,format=I420,pixel-aspect-ratio=1/1,");
        QString raw = rawPrefix + modes;
        raw.replace(QLatin1Char(';'), QLatin1Char(';') + rawPrefix);
        QString advertised = QStringLiteral("image/jpeg,") + modes;
        advertised.replace(QLatin1Char(';'), QStringLiteral(";image/jpeg,"));
        if (!jpeg)
            advertised = raw;
        GstCaps *caps = gst_caps_from_string(advertised.toUtf8().constData());
        GstCaps *chosen = SfuMediaEngine::preferredCameraCaps(caps, true);
        QVERIFY(chosen);
        QVERIFY(gst_caps_is_subset(chosen, caps));
        const GstCaps *entryCaps = probeFormats ? chosen : nullptr;
        const QString entry = jpeg ? SfuMediaEngine::cameraJpegEntry(entryCaps)
                                   : SfuMediaEngine::directCameraRawEntry(entryCaps);
        gst_caps_unref(chosen);
        gst_caps_unref(caps);
        const QString source = QStringLiteral("videotestsrc num-buffers=3");
        const QString fixedEntry = QStringLiteral("capsfilter name=syntheticmodes caps=\"%1\"").arg(raw)
            + (jpeg ? QStringLiteral(" ! jpegenc ! capsfilter caps=\"%1\"")
                          .arg(advertised) : QString())
            + QStringLiteral(" ! ") + entry;
        QString description = SfuMediaEngine::videoPipelineDescription(
            source, SfuMediaEngine::cameraRateStage(false),
            SfuMediaEngine::cameraLimitsCaps(false, 720),
            SfuMediaEngine::cameraEncoderStage(720), QString(), 1u,
            QStringLiteral("videoconvert ! videoscale"), fixedEntry);
        description += QStringLiteral(" ! fakesink sync=false");
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
        const QString parseError = QString::fromUtf8(error ? error->message : "");
        g_clear_error(&error);
        QVERIFY2(pipeline, qPrintable(parseError));
        GstBus *bus = gst_element_get_bus(pipeline);
        const GstStateChangeReturn state = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        const bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        QString detail;
        if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(message, &error, nullptr);
            detail = QString::fromUtf8(error->message);
            g_clear_error(&error);
        }
        GstElement *capture = gst_bin_get_by_name(GST_BIN(pipeline), "syntheticmodes");
        GstPad *capturePad = gst_element_get_static_pad(capture, "src");
        GstCaps *negotiated = gst_pad_get_current_caps(capturePad);
        int actualWidth = 0, actualHeight = 0, numerator = 0, denominator = 1;
        if (negotiated) {
            const GstStructure *st = gst_caps_get_structure(negotiated, 0);
            gst_structure_get_int(st, "width", &actualWidth);
            gst_structure_get_int(st, "height", &actualHeight);
            gst_structure_get_fraction(st, "framerate", &numerator, &denominator);
            gst_caps_unref(negotiated);
        }
        gst_object_unref(capturePad);
        gst_object_unref(capture);
        if (message)
            gst_message_unref(message);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        QVERIFY2(parseError.isEmpty(), qPrintable(parseError));
        QVERIFY(state != GST_STATE_CHANGE_FAILURE);
        QVERIFY2(eos, qPrintable(detail.isEmpty() ? description : detail));
        QCOMPARE(actualWidth, width);
        QCOMPARE(actualHeight, height);
        QCOMPARE(numerator, captureFps * denominator);
    }

    void aMissingChosenCameraIsRefusedBeforeAnyCaptureOpens()
    {
        SfuMediaEngine engine;
        engine.setPreferredDevices({QString(), QString(), true}, {}, {});
        QSignalSpy failed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        engine.publishVideo(QStringLiteral("missing"), false, -1, -1);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("camera_unavailable"));
        QVERIFY(!engine.hasPublishedBinForTest(QStringLiteral("missing")));
        engine.stop();
    }

    void cameraMmapRequiresStreamingCapabilities()
    {
#ifdef Q_OS_LINUX
        QCOMPARE(SfuMediaEngine::cameraIoMode(V4L2_CAP_VIDEO_CAPTURE
                                                | V4L2_CAP_STREAMING),
                 QStringLiteral("mmap"));
        QCOMPARE(SfuMediaEngine::cameraIoMode(V4L2_CAP_VIDEO_CAPTURE
                                                | V4L2_CAP_STREAMING
                                                | V4L2_CAP_READWRITE),
                 QStringLiteral("mmap"));
        QCOMPARE(SfuMediaEngine::cameraIoMode(V4L2_CAP_VIDEO_CAPTURE
                                                | V4L2_CAP_READWRITE),
                 QStringLiteral("auto"));
        QCOMPARE(SfuMediaEngine::cameraIoMode(0), QStringLiteral("auto"));
        QVERIFY(!SfuMediaEngine::cameraSource().contains(QStringLiteral("io-mode=mmap")));
#endif
    }

    void onlyV4l2ResourceErrorsAreCameraResourceErrors()
    {
#ifndef Q_OS_LINUX
        QSKIP("V4L2 and PipeWire camera sources are Linux only");
#else
        for (const char *factory : {"v4l2src", "pipewiresrc"}) {
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.setVideoSourceForTest(QStringLiteral("appsrc is-live=true format=time"));
            engine.start();
            QSignalSpy failed(&engine, &SfuMediaEngine::publishFailed);
            engine.publishVideo(QStringLiteral("cam"), false, -1, -1);
            GstElement *bin = engine.publishedBinForTest(QStringLiteral("cam"));
            QVERIFY(bin);
            GstElement *source = gst_element_factory_make(factory, "capsrcerror");
            QVERIFY2(source, factory);
            gst_element_set_locked_state(source, TRUE);
            QVERIFY(gst_bin_add(GST_BIN(bin), source));
            GError *error = g_error_new_literal(
                GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_FAILED, "synthetic failure");
            gst_element_post_message(source, gst_message_new_error(
                GST_OBJECT(source), error, "synthetic"));
            g_error_free(error);
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
            QCOMPARE(failed.at(0).at(1).toString(),
                     QByteArray(factory) == "v4l2src" ? QStringLiteral("camera_resource")
                                                     : QStringLiteral("camera_failed"));
            engine.stop();
        }
#endif
    }

    void aCameraResourceErrorIsReportedByKind()
    {
        QCOMPARE(SfuMediaEngine::cameraPublishErrorCategory(false, true),
                 QStringLiteral("camera_busy"));
        QCOMPARE(SfuMediaEngine::cameraPublishErrorCategory(false, false),
                 QStringLiteral("camera_failed"));
        QCOMPARE(SfuMediaEngine::cameraPublishErrorCategory(true, true),
                 QStringLiteral("screen_share_failed"));

        struct Case { bool resource; bool busy; const char *category; };
        for (const Case c : { Case{ true, true, "camera_busy" },
                              Case{ true, false, "camera_resource" },
                              Case{ false, false, "camera_failed" } }) {
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.setVideoSourceForTest(
                QStringLiteral("appsrc is-live=true format=time"));
            QSignalSpy trackFailed(&engine, &SfuMediaEngine::publishFailed);
            engine.start();
            engine.publishVideo(QStringLiteral("cid-camera"),
false, -1, -1);
            QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-camera")));
            engine.handlePublishError(QStringLiteral("cid-camera"), c.resource,
                                      c.busy);
            QCOMPARE(trackFailed.count(), 1);
            QCOMPARE(trackFailed.at(0).at(1).toString(),
                     QString::fromLatin1(c.category));
            engine.stop();
        }
    }

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
false, -1, -1);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-camera")));
        QTRY_COMPARE_WITH_TIMEOUT(trackFailed.count(), 1, 10000);
        QCOMPARE(trackFailed.at(0).at(0).toString(),
                 QStringLiteral("cid-camera"));
        QCOMPARE(trackFailed.at(0).at(1).toString(),
                 QStringLiteral("camera_no_frames"));
        QTest::qWait(3500);
        QCOMPARE(trackFailed.count(), 1);
        QCOMPARE(failed.count(), 0);
        engine.stop();
    }

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
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        QElapsedTimer took;
        took.start();
        engine.publishVideo(cid,false,-1, fd);
        const qint64 ms = took.elapsed();
        QVERIFY(engine.hasPublishedBinForTest(cid));
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("publishVideo() blocked %1 ms on "
                                           "the camera's start")
                                .arg(ms)));
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
false,-1, fd);
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
            QTest::qWait(kSlowStartCamMs + 500);
        }
    }

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
        engine.publishVideo(cid,false,-1, fd);
        GstElement *bin = engine.publishedBinForTest(cid);
        QVERIFY(bin);
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
        QTest::qWait(2 * kSlowStartCamMs + 500);
    }

    void theMicrophoneIsStartedOffTheGuiThread()
    {
        QVERIFY(registerSlowStartMic());
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine.start();
        const QString cid = QStringLiteral("cid-slow-mic");
        QElapsedTimer took;
        took.start();
        engine.publishAudio(cid);
        const qint64 ms = took.elapsed();
        QVERIFY(engine.hasPublishedBinForTest(cid));
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("publishAudio() blocked %1 ms on "
                                           "the microphone's start")
                                .arg(ms)));
        QVERIFY(engine.microphoneStartingForTest());
        QTRY_VERIFY_WITH_TIMEOUT(!engine.microphoneStartingForTest(),
                                 3 * kSlowStartCamMs);
        QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneStartsReportedForTest(), 1,
                                  2000);
        GstElement *bin = engine.publishedBinForTest(cid);
        QVERIFY(bin);
        QTRY_VERIFY_WITH_TIMEOUT(
            [bin] {
                GstState state = GST_STATE_VOID_PENDING;
                gst_element_get_state(bin, &state, nullptr, 0);
                return state == GST_STATE_PLAYING;
            }(),
            5000);
        engine.stop();
    }

    void aCallLeftWhileTheMicrophoneStartsNeitherWaitsNorHearsFromIt()
    {
        QVERIFY(registerSlowStartMic());
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine.start();
        engine.setEncryptionRequired(true);
        engine.setOutboundKey(3, QByteArray(32, 'k'));
        engine.publishAudio(QStringLiteral("cid-slow-mic"));
        QVERIFY(engine.microphoneStartingForTest());

        QElapsedTimer took;
        took.start();
        engine.stop();
        const qint64 ms = took.elapsed();
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("stop() waited %1 ms for a "
                                           "microphone still starting")
                                .arg(ms)));
        const quint64 encryptedAtStop = engine.framesEncrypted();
        engine.start();
        QTest::qWait(kSlowStartCamMs + 1500);
        QCOMPARE(engine.microphoneStartsReportedForTest(), 0);
        QVERIFY2(engine.framesEncrypted() == encryptedAtStop,
                 "the detached microphone encrypted frames after its call "
                 "ended: its probe is not gated");
        engine.stop();
    }

    void anEngineDestroyedWhileTheMicrophoneStartsLeavesItInert()
    {
        QVERIFY(registerSlowStartMic());
        auto *engine = new SfuMediaEngine;
        engine->setTestSourceMode(true);
        engine->setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine->start();
        engine->setEncryptionRequired(true);
        engine->setOutboundKey(3, QByteArray(32, 'k'));
        engine->publishAudio(QStringLiteral("cid-slow-mic"));
        QElapsedTimer took;
        took.start();
        engine->stop();
        const qint64 ms = took.elapsed();
        delete engine;
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("stop() waited %1 ms for a "
                                           "microphone still starting")
                                .arg(ms)));
        QTest::qWait(kSlowStartCamMs + 1500);
    }

    void aMicrophoneSwitchDuringItsStartWaitsForIt()
    {
        QVERIFY(registerSlowStartMic());
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine.start();
        engine.publishAudio(QStringLiteral("cid-slow-mic"));
        QVERIFY(engine.microphoneStartingForTest());
        engine.setMicrophoneSourceForTest(
            QStringLiteral("audiotestsrc is-live=true wave=sine freq=330 "
                           "volume=0.05"));
        engine.refreshMicrophoneCapture();
        QVERIFY2(engine.microphoneSwapDeferredForTest(),
                 "a switch changed the front of a bin still starting");
        QCOMPARE(engine.microphoneSwapsForTest(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.microphoneSwapDeferredForTest(),
                                 3 * kSlowStartCamMs);
        QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneSwapsForTest(), 1, 5000);
        engine.stop();
    }

    void theMicrophoneRtpVerdictIsGivenAfterASlowStart()
    {
        QVERIFY(registerSlowStartMic());
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneRtpCheckMsForTest(1000);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine.start();
        engine.publishAudio(QStringLiteral("cid-slow-mic"));
        QTRY_COMPARE_WITH_TIMEOUT(engine.lastMicrophoneRtpVerdictForTest(),
                                  QStringLiteral("starting"), 2000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            engine.lastMicrophoneRtpVerdictForTest()
                != QStringLiteral("starting"),
            "the watchdog never gave a verdict after the slow start", 6000);
        QCOMPARE(engine.lastMicrophoneRtpVerdictForTest(),
                 QStringLiteral("flowing"));
        engine.stop();
    }

    void aMicrophoneRestartDuringItsStartRunsAfterIt()
    {
        QVERIFY(registerSlowStartMic());
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine.start();
        engine.publishAudio(QStringLiteral("cid-slow-mic"));
        QVERIFY(engine.microphoneStartingForTest());
        const int before = engine.microphoneRestartsForTest();
        engine.restartMicrophoneForTest();
        QCOMPARE(engine.microphoneRestartsForTest(), before);
        QVERIFY2(engine.microphoneRestartPendingForTest(),
                 "a deferred restart was marked done before it ran");
        QTRY_VERIFY_WITH_TIMEOUT(!engine.microphoneStartingForTest(),
                                 3 * kSlowStartCamMs);
        QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneRestartsForTest(),
                                  before + 1, 2000);
        QVERIFY(!engine.microphoneRestartPendingForTest());
        engine.stop();
        QTest::qWait(kSlowStartCamMs + 500);
    }

    void aNoiseModeChangeDuringTheMicrophoneStartConfiguresItsDspInPlace()
    {
        GstElementFactory *factory = gst_element_factory_find("webrtcdsp");
        if (!factory)
            QSKIP("webrtcdsp is not in this GStreamer");
        gst_object_unref(factory);
        QVERIFY(registerSlowStartMic());
        const QString cid = QStringLiteral("cid-slow-mic");
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setNoiseSuppressionMode(calls::noise::Mode::WebRtc);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine.start();
        engine.publishAudio(cid);
        QVERIFY(engine.microphoneStartingForTest());
        GstElement *bin = engine.publishedBinForTest(cid);
        QVERIFY(bin);
        GstElement *dspBefore = gst_bin_get_by_name(GST_BIN(bin), "micdsp");
        QVERIFY(dspBefore);
        engine.setNoiseSuppressionMode(calls::noise::Mode::Off);
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdenoise"), QStringLiteral("mode")),
                 QStringLiteral("off"));
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdsp"),
                     QStringLiteral("noise-suppression")),
                 QStringLiteral("false"));
        QTRY_VERIFY_WITH_TIMEOUT(!engine.microphoneNoiseModeDeferredForTest(),
                                 3 * kSlowStartCamMs);
        QTest::qWait(300);
        GstElement *dspAfter = gst_bin_get_by_name(GST_BIN(bin), "micdsp");
        QVERIFY2(dspAfter == dspBefore,
                 "webrtcdsp was replaced for a change it had taken in time");
        QCOMPARE(engine.publishedBinPropertyForTest(
                     cid, QStringLiteral("micdsp"),
                     QStringLiteral("noise-suppression")),
                 QStringLiteral("false"));
        if (dspAfter)
            gst_object_unref(dspAfter);
        gst_object_unref(dspBefore);
        engine.stop();
    }

    void aReplacedMicrophoneIsNeverDisposedWhileItRuns()
    {
        QVERIFY(registerSlowStopMic());
        LiveDisposeCounter disposes;
        {
            SfuMediaEngine engine;
            engine.setTestSourceMode(true);
            engine.setMicrophoneSourceForTest(
                QStringLiteral("lightningslowstopmic"));
            engine.start();
            engine.publishAudio(QStringLiteral("cid-mic"));
            QTRY_VERIFY_WITH_TIMEOUT(!engine.microphoneStartingForTest(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(engine.microphoneCapturedForTest() > 5,
                                     5000);
            engine.setMicrophoneSourceForTest(
                QStringLiteral("audiotestsrc is-live=true wave=sine freq=330 "
                               "volume=0.05"));
            engine.refreshMicrophoneCapture();
            QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneSwapsForTest(), 1, 5000);
            engine.stop();
            QTest::qWait(4 * kSlowStopMicMs);
        }
        QTest::qWait(kSlowStopMicMs);
        QCOMPARE(LiveDisposeCounter::count().load(), 0);
    }

    void aDeviceSwitchReplacesAStuckMicrophoneStart()
    {
        QVERIFY(registerStuckStartMic());
        const StuckStartRelease releaseAtEnd;
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        sender.setMicrophoneStartAbandonMsForTest(500);
        sender.setMicrophoneSourceForTest(
            QStringLiteral("lightningstuckstartmic"));
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        sender.start();
        receiver.start();
        QElapsedTimer since;
        since.start();
        sender.publishAudio(QStringLiteral("first"));
        sender.setMicrophoneSourceForTest(
            QStringLiteral("audiotestsrc is-live=true wave=sine freq=330 "
                           "volume=0.05"));
        sender.refreshMicrophoneCapture();
        QVERIFY(sender.microphoneSwapDeferredForTest());
        QTRY_COMPARE_WITH_TIMEOUT(sender.microphoneSwapsForTest(), 1, 5000);
        QVERIFY2(!sender.microphoneStartingForTest(),
                 "the switch took the stuck start over but the engine still "
                 "treats the microphone as starting");
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrived.isEmpty(),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            kStuckStartMicMs - 2000);
        const quint64 captured = sender.microphoneCapturedForTest();
        QTest::qWait(300);
        QVERIFY2(sender.microphoneCapturedForTest() > captured,
                 "the capture that took over delivers nothing");
        QVERIFY(since.elapsed() < kStuckStartMicMs);
        sender.stop();
        receiver.stop();
    }

    void aLiveMicrophoneFailedDuringAnAbandonedSwitchIsRestarted()
    {
        QVERIFY(registerStuckStartMic());
        const StuckStartRelease releaseAtEnd;
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
        sender.start();
        receiver.start();
        sender.publishAudio(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrived.isEmpty(),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            45000);
        sender.setMicrophoneSourceForTest(
            QStringLiteral("audiotestsrc is-live=true wave=sine freq=330 "
                           "volume=0.05"));
        sender.refreshMicrophoneCapture();
        QTRY_COMPARE_WITH_TIMEOUT(sender.microphoneSwapsForTest(), 1, 5000);
        sender.setMicrophoneSwapTimeoutForTest(400);
        sender.setMicrophoneSourceForTest(
            QStringLiteral("lightningstuckstartmic"));
        sender.refreshMicrophoneCapture();
        const int restartsDone = sender.microphoneSourceRestartsDoneForTest();
        sender.restartMicrophoneForTest();
        QCOMPARE(sender.microphoneSourceRestartsDoneForTest(), restartsDone);
        QTRY_COMPARE_WITH_TIMEOUT(sender.microphoneSwapsAbandonedForTest(), 1,
                                  3000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            sender.microphoneSourceRestartsDoneForTest() > restartsDone,
            "the live microphone that failed during the switch was never "
            "restarted",
            3000);
        const quint64 captured = sender.microphoneCapturedForTest();
        QTest::qWait(300);
        QVERIFY2(sender.microphoneCapturedForTest() > captured,
                 "the restarted microphone delivers nothing");
        sender.stop();
        receiver.stop();
    }

    void aRestartAfterATakeoverKeepsTheNewMicrophoneLive()
    {
        QVERIFY(registerStuckStartMic());
        const StuckStartRelease releaseAtEnd;
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        sender.setMicrophoneStartAbandonMsForTest(300);
        sender.setMicrophoneSourceForTest(
            QStringLiteral("lightningstuckstartmic"));
        QString failure;
        wireLoopback(sender, receiver, this, &failure);
        QStringList arrived;
        connect(&receiver, &SfuMediaEngine::remoteTrackAdded, this,
                [&](const QString &streamId, const QString &,
                    const QString &) { arrived << streamId; });
        sender.start();
        receiver.start();
        QElapsedTimer since;
        since.start();
        sender.publishAudio(QStringLiteral("first"));
        sender.setMicrophoneSourceForTest(
            QStringLiteral("audiotestsrc is-live=true wave=sine freq=330 "
                           "volume=0.05"));
        sender.refreshMicrophoneCapture();
        QTRY_COMPARE_WITH_TIMEOUT(sender.microphoneSwapsForTest(), 1, 2000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            !arrived.isEmpty(),
            qPrintable(QStringLiteral("no track arrived; failure=%1")
                           .arg(failure)),
            kStuckStartMicMs - 3000);
        const int restartsDone = sender.microphoneSourceRestartsDoneForTest();
        sender.restartMicrophoneForTest();
        QTRY_VERIFY_WITH_TIMEOUT(
            sender.microphoneSourceRestartsDoneForTest() > restartsDone, 1500);
        QVERIFY2(since.elapsed() < kStuckStartMicMs,
                 "the restart only finished once the stuck start had");
        const quint64 captured = sender.microphoneCapturedForTest();
        QTest::qWait(300);
        QVERIFY2(sender.microphoneCapturedForTest() > captured,
                 "the restart stopped the live microphone");
        sender.stop();
        receiver.stop();
    }

    void aSwitchToASlowMicrophoneNeverBlocksAndDiesWithTheCall()
    {
        QVERIFY(registerSlowStartMic());
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.start();
        engine.publishAudio(QStringLiteral("cid-mic"));
        QTRY_VERIFY_WITH_TIMEOUT(!engine.microphoneStartingForTest(), 5000);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        QElapsedTimer took;
        took.start();
        engine.refreshMicrophoneCapture();
        engine.stop();
        QVERIFY2(took.elapsed() < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("a switch and a hang-up waited %1 "
                                           "ms on the new capture's start")
                                .arg(took.elapsed())));
        QTest::qWait(kSlowStartCamMs + 1000);
        QCOMPARE(engine.microphoneSwapsForTest(), 0);
    }

    void unpublishingTheMicrophoneDuringItsStartDoesNotWait()
    {
        QVERIFY(registerSlowStartMic());
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setMicrophoneSourceForTest(
            QStringLiteral("lightningslowstartmic"));
        engine.start();
        const QString cid = QStringLiteral("cid-slow-mic");
        engine.publishAudio(cid);
        QVERIFY(engine.microphoneStartingForTest());
        QElapsedTimer took;
        took.start();
        engine.unpublish(cid);
        QVERIFY2(took.elapsed() < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("unpublish() waited %1 ms for the "
                                           "start")
                                .arg(took.elapsed())));
        QVERIFY(!engine.hasPublishedBinForTest(cid));
        QVERIFY(!engine.microphoneStartingForTest());
        QTest::qWait(kSlowStartCamMs + 1000);
        QCOMPARE(engine.microphoneStartsReportedForTest(), 0);
        engine.stop();
    }

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
        engine.publishVideo(cid,false,-1,
-1);
        QVERIFY(engine.hasPublishedBinForTest(cid));
        QCOMPARE(engine.pendingTeardownsForTest(), 0);
        engine.unpublish(cid);
        QVERIFY2(engine.pendingTeardownsForTest() > 0,
                 "a direct camera's teardown was taken off the counted path");
        QTRY_COMPARE_WITH_TIMEOUT(engine.pendingTeardownsForTest(), 0,
                                  4 * kSlowStartCamMs);
        engine.stop();
    }

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
false,-1, fd);
        QElapsedTimer took;
        took.start();
        engine->stop();
        const qint64 ms = took.elapsed();
        delete engine;
        QVERIFY2(ms < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("stop() waited %1 ms for a camera "
                                           "still starting")
                                .arg(ms)));
        QTest::qWait(kSlowStartCamMs + 1500);
    }

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
false,-1, fd);
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

    void aCameraThatDeliversOrAShareThatWaitsIsNotReported()
    {
        SfuMediaEngine engine;
        engine.setTestSourceMode(true);
        engine.setCameraFirstFrameTimeoutForTest(1000);
        QSignalSpy trackFailed(&engine, &SfuMediaEngine::publishFailed);
        engine.start();
        engine.publishVideo(QStringLiteral("cid-camera"),
false, -1, -1);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-camera")));
        engine.setVideoSourceForTest(
            QStringLiteral("appsrc is-live=true format=time"));
        engine.publishVideo(QStringLiteral("cid-share"),
true, -1, -1);
        QVERIFY(engine.hasPublishedBinForTest(QStringLiteral("cid-share")));
        engine.publishVideo(QStringLiteral("cid-gone"),
false, -1, -1);
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

        sender.setVideoSourceForTest(
            QStringLiteral("appsrc is-live=true format=time"));
        sender.publishVideo(QStringLiteral("cid-camera"),false,
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
        sender.setVideoSourceForTest(QString());
        sender.publishVideo(QStringLiteral("cid-share"),true,
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
        for (int i = 0; i < 6 && trackFailed.isEmpty(); ++i) {
            engine.handleCaptureError(cid);
            QTest::qWait(1500);
        }
        QCOMPARE(trackFailed.count(), 1);
        QCOMPARE(engine.microphoneRestartsForTest(), 5);
        QTest::qWait(1800);
        QCOMPARE(recovered.count(), 0);
        QTimer feed;
        QObject::connect(&feed, &QTimer::timeout, &engine,
                         [&engine] { engine.addMicCapturedForTest(5); });
        feed.start(50);
        QTRY_COMPARE_WITH_TIMEOUT(recovered.count(), 1, 5000);
        feed.stop();
        QCOMPARE(recovered.at(0).at(0).toString(), cid);
        QCOMPARE(engine.microphoneRestartsForTest(), 0);
        engine.handleCaptureError(cid);
        QTRY_COMPARE_WITH_TIMEOUT(engine.microphoneRestartsForTest(), 1, 3000);
        QCOMPARE(trackFailed.count(), 1);
        engine.stop();
    }

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
        QTRY_VERIFY2_WITH_TIMEOUT(
            sender.micPeakDbForTest() > -30.0
                && sender.micPeakDbForTest() < 0.0,
            qPrintable(QStringLiteral("the meter read %1 dBFS")
                           .arg(sender.micPeakDbForTest())),
            5000);

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

    void aSpeakerSwitchNeverWaitsOnTheOutputOpening()
    {
        QVERIFY(registerSlowOpenSink());
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setSpeakerSinkForTest(QStringLiteral("lightningslowopensink"));
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
        QElapsedTimer took;
        took.start();
        receiver.setPreferredDevices({}, {}, {QStringLiteral("other-output"),
                                              QStringLiteral("Other output")});
        QCoreApplication::processEvents();
        QVERIFY2(took.elapsed() < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("the speaker switch held the GUI "
                                           "thread %1 ms")
                                .arg(took.elapsed())));
        QTRY_COMPARE_WITH_TIMEOUT(receiver.speakerSwapsForTest(), 1,
                                  3 * kSlowStartCamMs);
        GstElement *bin = receiver.receiveBinForTest(QStringLiteral("first"));
        GstElement *outsink = gst_bin_get_by_name(GST_BIN(bin), "outsink");
        QVERIFY(outsink);
        static std::atomic<int> reachedNew{0};
        reachedNew.store(0);
        countBuffersAt(outsink, &reachedNew);
        gst_object_unref(outsink);
        QTRY_VERIFY2_WITH_TIMEOUT(reachedNew.load() > 10,
                                  "nothing reached the new output", 10000);

        receiver.setPreferredDevices({}, {}, {QStringLiteral("third-output"),
                                              QStringLiteral("Third output")});
        QCoreApplication::processEvents();
        took.restart();
        receiver.stop();
        QVERIFY2(took.elapsed() < kSlowStartCamMs / 2,
                 qPrintable(QStringLiteral("stop() waited %1 ms for an output "
                                           "still opening")
                                .arg(took.elapsed())));
        QTest::qWait(kSlowStartCamMs + 1000);
        QCOMPARE(receiver.speakerSwapsForTest(), 1);
        sender.stop();
    }

    void twoSpeakerPicksInARowLeaveOneOutputAndTheLastWins()
    {
        QVERIFY(registerSlowOpenSink());
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
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
        receiver.setSpeakerSinkForTest(QStringLiteral("lightningslowopensink"));
        receiver.setPreferredDevices({}, {}, {QStringLiteral("output-a"),
                                              QStringLiteral("Output A")});
        receiver.setSpeakerSinkForTest(QStringLiteral("fakesink sync=false"));
        receiver.setPreferredDevices({}, {}, {QStringLiteral("output-b"),
                                              QStringLiteral("Output B")});
        QTRY_COMPARE_WITH_TIMEOUT(receiver.speakerSwapsForTest(), 1,
                                  3 * kSlowStartCamMs);
        QTest::qWait(500);
        QCOMPARE(receiver.speakerSwapsForTest(), 1);
        GstElement *bin = receiver.receiveBinForTest(QStringLiteral("first"));
        QVERIFY(bin);
        int outputs = 0;
        QString factory;
        GstIterator *it = gst_bin_iterate_recurse(GST_BIN(bin));
        GValue item = G_VALUE_INIT;
        while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
            auto *element = GST_ELEMENT(g_value_get_object(&item));
            gchar *name = gst_element_get_name(element);
            if (g_strcmp0(name, "outsink") == 0) {
                ++outputs;
                if (GstElementFactory *f = gst_element_get_factory(element))
                    factory = QString::fromUtf8(GST_OBJECT_NAME(f));
            }
            g_free(name);
            g_value_reset(&item);
        }
        g_value_unset(&item);
        gst_iterator_free(it);
        QCOMPARE(outputs, 1);
        QCOMPARE(factory, QStringLiteral("fakesink"));
        int sinks = 0;
        GstIterator *sinkIt = gst_bin_iterate_sinks(GST_BIN(bin));
        GValue sinkItem = G_VALUE_INIT;
        while (gst_iterator_next(sinkIt, &sinkItem) == GST_ITERATOR_OK) {
            ++sinks;
            g_value_reset(&sinkItem);
        }
        g_value_unset(&sinkItem);
        gst_iterator_free(sinkIt);
        QVERIFY2(sinks == 1, "an output was orphaned in the chain");
        receiver.stop();
        sender.stop();
    }

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
        QVERIFY2(seen.loudest.load() < 60,
                 qPrintable(QStringLiteral("loudest level %1 -dBov")
                                .arg(seen.loudest.load())));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

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

        const int before = second.load();
        QTest::qWait(3000);
        QVERIFY2(second.load() - before > 50,
                 qPrintable(QStringLiteral(
                     "the other track delivered %1 buffers in the 3 s after "
                     "one sink failed: the failure reached the transport")
                                .arg(second.load() - before)));
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

    void aReceiveChainThatRecoversEachTimeIsNeverGivenUp()
    {
        constexpr int kRestarts = 7;
        std::atomic<int> played[kRestarts + 1]{};
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(20);
        receiver.setFreshRebuildRunAfterForTest(300);
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

        for (int i = 1; i <= kRestarts; ++i) {
            GstElement *bin = receiver.receiveBinForTest(QStringLiteral("first"));
            GstElement *sink = firstSinkOf(bin);
            QVERIFY2(sink, qPrintable(QStringLiteral("no sink before restart %1")
                                          .arg(i)));
            failLikeADisconnectedSoundServer(sink);
            QTRY_VERIFY2_WITH_TIMEOUT(
                receiver.receiveBinForTest(QStringLiteral("first")) != nullptr
                    && receiver.receiveBinForTest(QStringLiteral("first"))
                        != bin,
                qPrintable(QStringLiteral("restart %1 was not rebuilt; "
                                          "notices=%2")
                               .arg(i)
                               .arg(lost.count())),
                10000);
            GstElement *rebuilt =
                firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first")));
            QVERIFY(rebuilt);
            countBuffersAt(rebuilt, &played[i]);
            QTRY_VERIFY_WITH_TIMEOUT(played[i].load() > 20, 10000);
            QTest::qWait(400);
        }
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), kRestarts);
        QCOMPARE(lost.count(), 0);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    void aRebuiltChainThatNeverRendersStillUsesTheBudget()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(20);
        receiver.setFreshRebuildRunAfterForTest(100);
        receiver.rebuiltReceiveChainsNeverRenderForTest(true);
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

        for (int i = 1; i <= 5; ++i) {
            GstElement *bin = receiver.receiveBinForTest(QStringLiteral("first"));
            GstElement *sink = firstSinkOf(bin);
            QVERIFY2(sink, qPrintable(QStringLiteral("no sink before failure %1")
                                          .arg(i)));
            postSoundServerError(sink);
            QTRY_VERIFY2_WITH_TIMEOUT(
                receiver.receiveBinForTest(QStringLiteral("first")) != nullptr
                    && receiver.receiveBinForTest(QStringLiteral("first"))
                        != bin,
                qPrintable(QStringLiteral("failure %1 was not rebuilt").arg(i)),
                10000);
            QTest::qWait(300);
        }
        QCOMPARE(lost.count(), 0);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), 5);
        postSoundServerError(
            firstSinkOf(receiver.receiveBinForTest(QStringLiteral("first"))));
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 10000);
        QCOMPARE(lost.at(0).at(0).toBool(), true);
        QTest::qWait(500);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), 5);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

    void aTrackRetiredDuringItsRebuildStaysRetired()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
        receiver.setReceiveRebuildDelayForTest(20);
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
        QTRY_VERIFY_WITH_TIMEOUT(
            receiver.receiveBinForTest(QStringLiteral("first")) == nullptr,
            5000);
        QCOMPARE(receiver.receiveBinsForTest(), 1);
        {
            static std::atomic<int> criticals{0};
            criticals.store(0);
            const guint handler = g_log_set_handler(
                "GLib-GObject", G_LOG_LEVEL_CRITICAL,
                [](const gchar *, GLogLevelFlags, const gchar *, gpointer data) {
                    static_cast<std::atomic<int> *>(data)->fetch_add(1);
                }, &criticals);
            const auto restore = qScopeGuard([handler] {
                g_log_remove_handler("GLib-GObject", handler);
            });
            QVERIFY(receiver.streamInfoStats().isEmpty());
            receiver.resetStreamInfoBaselines();
            QCOMPARE(criticals.load(), 0);
        }
        QTest::qWait(300);
        QCOMPARE(receiver.retireAllReceiveBinsForTest(), 1);
        QCOMPARE(receiver.receiveBinsForTest(), 0);

        QTest::qWait(2500);
        QCOMPARE(receiver.receiveBinsForTest(), 0);
        QVERIFY(receiver.receiveBinForTest(QStringLiteral("first")) == nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(receiver.receiveVolumeElementsForTest(), 0,
                                  3000);
        sender.stop();
        receiver.stop();
    }

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

        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 10000);
        QCOMPARE(lost.at(0).at(0).toBool(), true);
        const int atGiveUp = receiver.receiveRebuildAttemptsForTest();
        QCOMPARE(atGiveUp, 5);
        QVERIFY(receiver.receiveBinForTest(QStringLiteral("first")) == nullptr);
        QTest::qWait(800);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp);

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
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 2, 5000);
        QCOMPARE(lost.at(1).at(0).toBool(), false);

        receiver.notifyAudioOutputsChanged(true);
        QTest::qWait(600);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp + 1);
        QCOMPARE(receiver.receiveRearmsForTest(), 1);
        QCOMPARE(receiver.receiveBinsForTest(), 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

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

        for (int i = 0; i < 3; ++i) {
            receiver.notifyAudioOutputsChanged(false);
            QTest::qWait(50);
        }
        QTest::qWait(600);
        QCOMPARE(receiver.receiveRebuildAttemptsForTest(), atGiveUp);
        QCOMPARE(receiver.receiveRearmsForTest(), 0);

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

    void anOutputChangeDuringTheRetriesBuysOneMoreAttempt()
    {
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
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

        sender.unpublish(QStringLiteral("first"));
        QTRY_VERIFY2_WITH_TIMEOUT(
            receiver.receiveBinsForTest() == 0,
            qPrintable(QStringLiteral("a retired track still holds %1 "
                                      "receive bin(s); failure=%2")
                           .arg(receiver.receiveBinsForTest())
                           .arg(failure)),
            15000);
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 1, 5000);

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
        QCOMPARE(receiver.receiveBinsForTest(), 0);
    }

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
        const quint64 before = receiver.framesDecrypted();
        QTRY_VERIFY_WITH_TIMEOUT(receiver.framesDecrypted() > before + 10,
                                 10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));

        sender.stop();
        receiver.stop();
    }

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
        QVERIFY(offers("ANY"));
        QVERIFY(offers("EMPTY"));
        QVERIFY(SfuMediaEngine::capsOfferJpeg(nullptr));
    }

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
        engine.publishVideo(cid,false,-1);
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

    void iceServerUrisTakeTheFormGstwebrtcniceParses()
    {
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
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(engine.teardownsWhileGatheringForTest(), 0);
        QVERIFY2(engine.retiresThatWaitedOnGatheringForTest() == 1,
                 "gathering had already ended when the call stopped, so this "
                 "run proved nothing: the silent TURN server did not hold it "
                 "open (no IPv4 host candidate to allocate from?)");
        QCOMPARE(engine.retireBoundExpiriesForTest(), 0);
    }

    void aStoppedPeersMediaStopsAtOnceWhileWebrtcbinWaits()
    {
        SilentUdpPort turn;
        if (turn.port() == 0)
            QSKIP("no loopback UDP socket for the silent TURN server");
        SfuMediaEngine sender;
        SfuMediaEngine receiver;
        sender.setTestSourceMode(true);
        receiver.setTestSourceMode(true);
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
                                  kRetiredOnceGatheringEndsWithinMs);
        QTRY_COMPARE_WITH_TIMEOUT(sender.retiringWebrtcForTest(), 0,
                                  kRetiredOnceGatheringEndsWithinMs);
        QCOMPARE(receiver.teardownsWhileGatheringForTest(), 0);
        QCOMPARE(sender.teardownsWhileGatheringForTest(), 0);
    }

};

QTEST_MAIN(SfuMediaEngineTest)
#include "SfuMediaEngineTest.moc"
