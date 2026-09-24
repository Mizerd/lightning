#include "calls/ShareAudioSources.h"

#include <QLoggingCategory>
#include <QRegularExpression>

#include <chrono>
#include <future>
#include <thread>

#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstBootstrap.h"
#include <gst/gst.h>
#endif

namespace {
Q_LOGGING_CATEGORY(lcShareAudio, "lightning.calls.shareaudio")

/// The caps all mixer inputs agree on: stereo 48 kHz, what the share-audio
/// encoder wants, so each branch resamples once.
constexpr const char *kMixCaps = "audio/x-raw,rate=48000,channels=2";

QString propString(const QVariantMap &props, const char *key)
{
    return props.value(QString::fromLatin1(key)).toString().trimmed();
}
} // namespace

namespace lightning::shareaudio {

QString mixerElementName()
{
    return QStringLiteral("shareaudiomix");
}

/// Digits only: the serial is interpolated into a
/// `gst_parse_bin_from_description` string, where a space or `!` would change
/// the pipeline. Checked here as well as in the reader because these builders
/// are public.
static bool serialIsTargetable(const QString &serial)
{
    static const QRegularExpression digits(QStringLiteral("\\A[0-9]{1,19}\\z"));
    return digits.match(serial).hasMatch();
}

Stream streamFromProperties(const QVariantMap &props)
{
    Stream s;
    const QString serial = propString(props, "object.serial");
    // Digits only; see serialIsTargetable().
    if (serialIsTargetable(serial))
        s.serial = serial;
    s.appName = propString(props, "application.name");
    s.nodeName = propString(props, "node.name");
    bool ok = false;
    const qint64 pid = propString(props, "application.process.id").toLongLong(&ok);
    s.pid = ok ? pid : -1;
    return s;
}

bool streamIsForeign(const QVariantMap &props, qint64 ourPid,
                     const QStringList &ourNames)
{
    // Exactly `Stream/Output/Audio`: `/Internal` nodes are PipeWire's own
    // plumbing and would double-count audio.
    if (propString(props, "media.class") != QLatin1String("Stream/Output/Audio"))
        return false;

    const Stream s = streamFromProperties(props);
    // No serial, nothing to target: pipewiresrc resolves stream nodes only by
    // serial, and anything else captures silence.
    if (s.serial.isEmpty())
        return false;

    // Exclude ourselves: our playback of the other participants is the echo.
    if (ourPid > 0 && s.pid == ourPid)
        return false;
    // The pid is the reliable check and the name a backup for nodes without
    // `application.process.id`. Every spelling is compared because PipeWire
    // uses the binary name ("lightning-matrix"), not applicationName().
    for (const QString &name : ourNames) {
        if (name.isEmpty())
            continue;
        if (s.appName.compare(name, Qt::CaseInsensitive) == 0
            || s.nodeName.compare(name, Qt::CaseInsensitive) == 0)
            return false;
    }

    // Virtual plumbing (`pw-loopback`, `filter-chain`) sets
    // `node.link-group`; its playback side repeats audio an application
    // already produced.
    if (!propString(props, "node.link-group").isEmpty())
        return false;

    return true;
}

QString applicationBranchDescription(const Stream &stream, int index)
{
    if (!serialIsTargetable(stream.serial))
        return QString();
    return QStringLiteral(
               // `min-buffers=1` is pinned: the default changed from 8 to 1
               // between gst-plugin-pipewire 1.4 and 1.6, and 8 fails to
               // negotiate with sources offering fewer. `on-disconnect=eos`
               // retires the branch when its application quits.
               "pipewiresrc name=shareapp%1 target-object=%2 min-buffers=1 "
               "do-timestamp=true on-disconnect=eos "
               "! queue max-size-time=200000000 leaky=downstream "
               // An explicit capsfilter, not bare caps: with nothing after
               // it (as in rescanShareAudioSources()), gst_parse reads trailing
               // caps as an element name ("no element \"audio\"").
               "! audioconvert ! audioresample ! capsfilter caps=\"%3\"")
        .arg(QString::number(index), stream.serial,
             QString::fromLatin1(kMixCaps));
}

QString mixedSourceDescription(const QList<Stream> &streams)
{
    QStringList chains;
    chains << QStringLiteral("audiomixer name=%1").arg(mixerElementName());
    // Silence floor: the aggregator needs a source that keeps producing, or a
    // share with nothing playing hands the encoder nothing. Silence adds
    // nothing to the mix.
    chains << QStringLiteral("audiotestsrc is-live=true wave=silence "
                             "! %1 ! %2.")
                  .arg(QString::fromLatin1(kMixCaps), mixerElementName());
    int index = 0;
    for (const Stream &s : streams) {
        const QString branch = applicationBranchDescription(s, index);
        if (branch.isEmpty())
            continue;
        chains << QStringLiteral("%1 ! %2.").arg(branch, mixerElementName());
        ++index;
    }
    // The mixer's output goes last so the caller's chain continues from it;
    // gst_parse treats the last-written chain as current.
    chains << QStringLiteral("%1.").arg(mixerElementName());
    return chains.join(QLatin1Char('\n'));
}

QString encodedTrackDescription(const QString &sourceDescription, quint32 ssrc)
{
    // Not the microphone chain: no `webrtcdsp` (AGC and noise suppression
    // damage music), stereo rather than mono, and music-grade Opus
    // (`audio-type=generic`, 128 kbit/s).
    //
    // Nothing may follow `%1` but a link: the per-application description
    // ends in a pad reference, which accepts no assignments.
    return QStringLiteral(
               // Bounded and leaky, like every live queue: a default queue
               // turns one encoder hiccup into permanent latency.
               "%1 ! queue max-size-buffers=0 max-size-bytes=0 "
               "max-size-time=100000000 leaky=downstream "
               "! audioconvert ! audioresample "
               "! audio/x-raw,channels=2,rate=48000 "
               // Its own valve, so muting share audio can never touch the
               // microphone. Not yet driven by any control.
               "! valve name=sharevalve drop=false "
               "! opusenc name=shareaudioenc audio-type=generic "
               "bitrate=128000 "
               "! rtpopuspay pt=111 ssrc=%2 "
               // The ssrc must be in the caps, as for the microphone bin, or
               // the offer has no a=ssrc and the SFU cannot attribute the RTP.
               "! capsfilter caps=\"application/x-rtp,media=audio,"
               "encoding-name=OPUS,payload=111,clock-rate=(int)48000,"
               "encoding-params=(string)2,ssrc=(uint)%2\"")
        .arg(sourceDescription, QString::number(ssrc));
}

SourceMonitor::~SourceMonitor()
{
    stop();
}

bool perApplicationCaptureAvailable()
{
    // Cached (the answer depends only on loaded plugins and the PipeWire
    // daemon) and bounded, because it runs on the GUI thread at call join:
    // SourceMonitor::start() ends in gst_device_monitor_start(), which is
    // synchronous and unbounded, and this unfiltered monitor starts every
    // provider, including PulseAudio's. Timing out answers false, the
    // existing "no per-application capture" state. A blocked probe cannot be
    // cancelled, so the worker is abandoned, at most once.
#ifndef HAVE_LIGHTNING_WEBRTC
    // No media engine: SourceMonitor::start() is the stub that returns false.
    return false;
#else
    static const bool available = [] {
        // Initialise on the caller's thread: ensureInitialised() is a
        // call_once, and if an abandoned worker were the first caller and
        // hung inside gst_init_check, every later caller would block on the
        // once_flag forever.
        if (!lightning::gst::ensureInitialised())
            return false;
        auto slot = std::make_shared<std::promise<bool>>();
        auto answer = slot->get_future();
        std::thread([slot] {
        // set_value inside the try: an exception escaping a detached thread
        // terminates, and a broken promise rethrows on the GUI thread.
            try {
                SourceMonitor probe;
                const bool ok = probe.start();
                probe.stop();
                slot->set_value(ok);
            } catch (...) {
                try {
                    slot->set_value(false);
                } catch (...) {
                }
            }
        }).detach();
        if (answer.wait_for(std::chrono::milliseconds(2500))
            != std::future_status::ready) {
            qCWarning(lcShareAudio)
                << "the PipeWire device monitor did not answer within 2500 ms;"
                << "per-application share audio is unavailable for this run";
            return false;
        }
        return answer.get();
    }();
    return available;
#endif
}

#ifdef HAVE_LIGHTNING_WEBRTC

namespace {
/// Whether `pipewiresrc` has the `on-disconnect` property the retirement path
/// needs. Checked because naming a missing property fails the whole parse.
bool pipewireSrcCanRetireItself()
{
    GstElementFactory *factory = gst_element_factory_find("pipewiresrc");
    if (!factory)
        return false;
    GstElement *probe = gst_element_factory_create(factory, nullptr);
    gst_object_unref(factory);
    if (!probe)
        return false;
    const bool has = g_object_class_find_property(G_OBJECT_GET_CLASS(probe),
                                                  "on-disconnect") != nullptr;
    gst_object_unref(probe);
    return has;
}

bool elementExists(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

QVariantMap propertiesOf(GstDevice *device)
{
    QVariantMap out;
    GstStructure *props = gst_device_get_properties(device);
    if (!props)
        return out;
    const int n = gst_structure_n_fields(props);
    for (int i = 0; i < n; ++i) {
        const gchar *name = gst_structure_nth_field_name(props, i);
        if (!name)
            continue;
        const GValue *value = gst_structure_get_value(props, name);
        if (!value)
            continue;
        // Read strings as strings: gst_value_serialize() quotes values with
        // spaces ("\"Google Chrome\""), which would break the name comparison
        // that excludes our own playback.
        if (G_VALUE_HOLDS_STRING(value)) {
            const gchar *text = g_value_get_string(value);
            out.insert(QString::fromUtf8(name),
                       QString::fromUtf8(text ? text : ""));
            continue;
        }
        gchar *text = gst_value_serialize(value);
        if (text) {
            out.insert(QString::fromUtf8(name), QString::fromUtf8(text));
            g_free(text);
        }
    }
    gst_structure_free(props);
    return out;
}
} // namespace

bool SourceMonitor::start()
{
    if (m_monitor)
        return true;
    // Initialise first: before the registry loads, factory lookups report
    // "no such element".
    lightning::gst::ensureInitialised();

    if (!gst_device_provider_factory_find("pipewiredeviceprovider")) {
        qCInfo(lcShareAudio) << "per-application share audio unavailable: no "
                                "PipeWire device provider in this build";
        return false;
    }
    if (!elementExists("audiomixer")) {
        qCInfo(lcShareAudio) << "per-application share audio unavailable: no "
                                "audiomixer element";
        return false;
    }
    if (!pipewireSrcCanRetireItself()) {
        qCInfo(lcShareAudio) << "per-application share audio unavailable: "
                                "pipewiresrc has no on-disconnect property, so "
                                "a departing application would stall the mixer";
        return false;
    }

    GstDeviceMonitor *monitor = gst_device_monitor_new();
    // No filter: gst_device_monitor_add_filter matches providers by their
    // advertised classes, and the PipeWire provider does not advertise
    // "Stream/Output/Audio", so such a filter makes start() fail. A caps
    // filter would drop streams by their negotiated caps. streamIsForeign()
    // selects by `media.class` instead.
    gst_device_monitor_add_filter(monitor, nullptr, nullptr);
    if (!gst_device_monitor_start(monitor)) {
        qCInfo(lcShareAudio) << "per-application share audio unavailable: the "
                                "device monitor would not start";
        gst_object_unref(monitor);
        return false;
    }
    // Flush the bus: the monitor posts DEVICE_ADDED/REMOVED for every play and
    // pause, each holding device refs, and nothing reads them (streams()
    // queries the current list).
    if (GstBus *bus = gst_device_monitor_get_bus(monitor)) {
        gst_bus_set_flushing(bus, TRUE);
        gst_object_unref(bus);
    }
    m_monitor = monitor;
    return true;
}

void SourceMonitor::stop()
{
    if (!m_monitor)
        return;
    GstDeviceMonitor *monitor = static_cast<GstDeviceMonitor *>(m_monitor);
    m_monitor = nullptr;
    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);
}

QList<Stream> SourceMonitor::streams(qint64 ourPid,
                                     const QStringList &ourNames) const
{
    QList<Stream> out;
    if (!m_monitor)
        return out;
    GList *devices =
        gst_device_monitor_get_devices(static_cast<GstDeviceMonitor *>(m_monitor));
    for (GList *l = devices; l; l = l->next) {
        GstDevice *device = GST_DEVICE(l->data);
        const QVariantMap props = propertiesOf(device);
        if (!streamIsForeign(props, ourPid, ourNames))
            continue;
        out.append(streamFromProperties(props));
    }
    g_list_free_full(devices, gst_object_unref);
    return out;
}

#else  // !HAVE_LIGHTNING_WEBRTC

bool SourceMonitor::start()
{
    return false;
}

void SourceMonitor::stop() {}

QList<Stream> SourceMonitor::streams(qint64, const QStringList &) const
{
    return {};
}

#endif // HAVE_LIGHTNING_WEBRTC

} // namespace lightning::shareaudio
