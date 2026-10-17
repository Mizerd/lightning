// Opt-in measurement: supply a hardware-free private graph and a separate
// 690 Hz application. The 1500 Hz far-end tone uses the REAL SFU receive bin
// in this process, including its production output sink (testSink=false).
#include "calls/GstBootstrap.h"
#include "calls/SfuMediaEngine.h"
#include "calls/ShareAudioSources.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <gst/app/gstappsink.h>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
#include <unistd.h>

namespace {
struct Pipeline {
    GstElement *element = nullptr;
    ~Pipeline()
    {
        if (element) {
            gst_element_set_state(element, GST_STATE_NULL);
            gst_object_unref(element);
        }
    }
};
GstElement *parse(const QString &description)
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (error) {
        fprintf(stderr, "parse failed: %s\n", error->message);
        g_error_free(error);
        if (pipeline)
            gst_object_unref(pipeline);
        return nullptr;
    }
    return pipeline;
}
double toneDb(const std::vector<float> &samples, double frequency)
{
    std::complex<double> sum{};
    double weight = 0;
    const double pi = std::acos(-1.0);
    for (size_t i = 0; i < samples.size(); ++i) {
        const double window = 0.5 - 0.5 * std::cos(2 * pi * i / (samples.size() - 1));
        sum += double(samples[i]) * window
            * std::polar(1.0, -2 * pi * frequency * i / 48000);
        weight += window;
    }
    return 20 * std::log10(std::max(2 * std::abs(sum) / weight, 1e-12));
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!qEnvironmentVariableIsSet("LIGHTNING_PRIVATE_AUDIO_PROBE")
        || !QDir::isAbsolutePath(runtime)
        || runtime == QStringLiteral("/run/user/%1").arg(getuid())
        || qEnvironmentVariable("PIPEWIRE_RUNTIME_DIR") != runtime
        || !qEnvironmentVariable("PULSE_SERVER").contains(runtime)
        || qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS")
               != QStringLiteral("unix:path=/nonexistent-gh26-bus")) {
        fprintf(stderr, "requires an acknowledged private graph and disabled session bus\n");
        return 2;
    }
    if (argc != 2 || !lightning::gst::ensureInitialised())
        return 2;
    const bool control = QByteArray(argv[1]) == "monitor-control";
    if (!control && QByteArray(argv[1]) != "system")
        return 2;
    Pipeline far{parse(QStringLiteral(
        "audiotestsrc is-live=true freq=1500 volume=0.3 ! audioconvert "
        "! audioresample ! audio/x-raw,rate=48000,channels=1 "
        "! opusenc ! rtpopuspay pt=111 name=remote"))};
    if (!far.element)
        return 1;
    QString error;
    GstElement *receive = SfuMediaEngine::buildReceiveBin(
        false, QStringLiteral("outvol_probe"), false, &error);
    if (!receive) {
        fprintf(stderr, "receive bin: %s\n", qPrintable(error));
        return 1;
    }
    gst_bin_add(GST_BIN(far.element), receive);
    GstElement *remote = gst_bin_get_by_name(GST_BIN(far.element), "remote");
    const bool linked = gst_element_link(remote, receive);
    gst_object_unref(remote);
    if (!linked || gst_element_set_state(far.element, GST_STATE_PLAYING)
                       == GST_STATE_CHANGE_FAILURE)
        return 1;
    QThread::sleep(2);
    const auto plan = lightning::shareaudio::systemCapture(app.applicationPid());
    if (!control && SfuMediaEngine::shareAudioAvailable() != plan.excludesUs) {
        fprintf(stderr, "engine availability disagrees with excluding capture\n");
        return 4;
    }
    fprintf(stderr, "capture kind=%d excludesUs=%d\n", int(plan.kind), plan.excludesUs);
    if (!control && plan.kind == lightning::shareaudio::SystemCapture::Kind::Unavailable) {
        fprintf(stderr, "share audio unavailable: no excluding capture\n");
        return 3;
    }
    QString source = plan.description;
    if (control) {
        // Deliberately broken pre-fix fallback: positive echo control.
        source = QStringLiteral("pulsesrc name=sharesrc device=@DEFAULT_MONITOR@");
    } else if (plan.kind == lightning::shareaudio::SystemCapture::Kind::PerApplication) {
        const auto listing = lightning::shareaudio::enumerate(
            app.applicationPid(), {app.applicationName(),
                QFileInfo(app.applicationFilePath()).completeBaseName()});
        for (const auto &stream : listing.streams)
            fprintf(stderr, "foreign stream=%s app=%s pid=%lld\n",
                    qPrintable(stream.serial), qPrintable(stream.appName), stream.pid);
        source = lightning::shareaudio::mixedSourceDescription(
            listing.streams, lightning::shareaudio::branchOptions());
    }
    const QString description = lightning::shareaudio::encodedTrackDescription(source, 1234)
        + QStringLiteral(" ! rtpopusdepay ! opusdec ! audioconvert ! audioresample "
                         "! audio/x-raw,format=F32LE,rate=48000,channels=1 "
                         "! appsink name=record sync=false max-buffers=256 drop=true");
    fprintf(stderr, "capture pipeline:\n%s\n", qPrintable(description));
    Pipeline share{parse(description)};
    if (!share.element)
        return 1;
    GstElement *record = gst_bin_get_by_name(GST_BIN(share.element), "record");
    gst_element_set_state(share.element, GST_STATE_PLAYING);
    std::vector<float> samples;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 7000) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(record), 100 * GST_MSECOND);
        if (!sample)
            continue;
        GstMapInfo map{};
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            if (timer.elapsed() > 2000) {
                const auto *pcm = reinterpret_cast<const float *>(map.data);
                samples.insert(samples.end(), pcm, pcm + map.size / sizeof(float));
            }
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
    }
    gst_object_unref(record);
    if (samples.size() < 3 * 48000) {
        fprintf(stderr, "too few captured samples: %zu\n", samples.size());
        return 1;
    }
    const double appDb = toneDb(samples, 690);
    const double farDb = toneDb(samples, 1500);
    printf("%s samples=%zu app690=%.2f dBFS far1500=%.2f dBFS\n",
           control ? "MONITOR CONTROL" : "SYSTEM", samples.size(), appDb, farDb);
    // Silence cannot pass as no echo: the external application must be present.
    return appDb > -25 && (control ? farDb > -25 : farDb < -75) ? 0 : 1;
}
