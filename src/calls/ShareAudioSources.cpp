#include "calls/ShareAudioSources.h"

#include <QLoggingCategory>
#include <QRegularExpression>

#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstBootstrap.h"
#include <gst/gst.h>
#endif

namespace {
Q_LOGGING_CATEGORY(lcShareAudio, "lightning.calls.shareaudio")

/// The one caps the mixer's inputs agree on. Stereo 48 kHz because that is
/// what the share-audio encoder wants anyway (`opusenc` at 128 kbit/s with
/// `audio-type=generic`), so every branch resamples once and the mixer sums
/// without converting again.
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

/// Digits, and nothing else. See streamFromProperties(): this value is
/// interpolated into a `gst_parse_bin_from_description` string, where a space
/// or a `!` would not be a bad target but a different pipeline.
///
/// CHECKED HERE TOO, not only where a Stream is read from PipeWire. These
/// builders are public and take a caller-built Stream; relying on every
/// caller having gone through the reader is call-site discipline, and the
/// point of a validation is that it does not depend on that.
static bool serialIsTargetable(const QString &serial)
{
    static const QRegularExpression digits(QStringLiteral("\\A[0-9]{1,19}\\z"));
    return digits.match(serial).hasMatch();
}

Stream streamFromProperties(const QVariantMap &props)
{
    Stream s;
    const QString serial = propString(props, "object.serial");
    // DIGITS ONLY, and refused otherwise. This value is interpolated into a
    // `gst_parse_bin_from_description` string, where a stray space or `!`
    // would not be a bad target but a different pipeline. It is a local
    // daemon's own counter and has never been anything but digits; the check
    // costs nothing and removes the question.
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
                     const QString &ourClientName)
{
    // EXACTLY `Stream/Output/Audio`. The `/Internal` suffix is PipeWire's own
    // plumbing — a split or converted node the session manager created — and
    // capturing it would double-count audio we already take from the
    // application that feeds it.
    if (propString(props, "media.class") != QLatin1String("Stream/Output/Audio"))
        return false;

    const Stream s = streamFromProperties(props);
    // Nothing to target. `pipewiresrc` resolves a stream node by serial and
    // by nothing else (measured — see the header), so a node without one is
    // not capturable, and pretending otherwise yields a branch that runs and
    // carries digital silence.
    if (s.serial.isEmpty())
        return false;

    // OURSELVES. This exclusion is the whole point of the file: Lightning's
    // playback of the other participants is what the far end was hearing
    // come back.
    if (ourPid > 0 && s.pid == ourPid)
        return false;
    if (!ourClientName.isEmpty()) {
        // The pid is the reliable half and the name is the belt: a node
        // created through a path that does not fill `application.process.id`
        // still carries a name, and shipping the echo again because one
        // property was absent is not a trade worth making.
        //
        // HOW MUCH TO TRUST IT: not much, and the fix does not rest on it.
        // What PipeWire records OUR playback under has not been captured
        // from a running Lightning — the application name is `matrix-client`
        // (src/main.cpp), and whether the node carries that, the binary
        // name, or something the audio sink chose is unverified. The pid
        // check is the guard; this is a second chance, not a second proof.
        if (s.appName.compare(ourClientName, Qt::CaseInsensitive) == 0
            || s.nodeName.compare(ourClientName, Qt::CaseInsensitive) == 0)
            return false;
    }

    // Virtual plumbing, not an application. `pw-loopback`, `filter-chain` and
    // friends mark both of their nodes with `node.link-group`; their playback
    // side carries audio some real application already produced, so taking it
    // as well would send the same sound twice.
    if (!propString(props, "node.link-group").isEmpty())
        return false;

    return true;
}

QString applicationBranchDescription(const Stream &stream, int index)
{
    if (!serialIsTargetable(stream.serial))
        return QString();
    return QStringLiteral(
               // `min-buffers=1` is PINNED, never inherited: the default moved
               // from 8 to 1 between gst-plugin-pipewire 1.4 and 1.6, and 8
               // cannot negotiate against a source offering fewer (§16).
               // `on-disconnect=eos` is what retires a branch when its
               // application quits — the mixer then treats that pad as done
               // instead of waiting on a source that will never speak again.
               "pipewiresrc name=shareapp%1 target-object=%2 min-buffers=1 "
               "do-timestamp=true on-disconnect=eos "
               "! queue max-size-time=200000000 leaky=downstream "
               // AN EXPLICIT `capsfilter`, NOT A BARE CAPS STRING, and that
               // is not a matter of taste. A description ENDING in
               // `! audio/x-raw,...` parses only when something follows it —
               // mixedSourceDescription() always appends `! sharemixer.`, so
               // it worked there and hid this. The dynamic path in
               // SfuMediaEngine::rescanShareAudioSources() hands this same
               // string to gst_parse_bin_from_description() with nothing
               // after it, and GStreamer then reads the caps as an ELEMENT
               // NAME: observed live on 2026-09-07 as
               // `could not build a branch for a new application: no element
               // "audio"`, once for every application that began playing
               // during a share. The initial set worked, so a share only ever
               // carried what happened to be playing when it started.
               "! audioconvert ! audioresample ! capsfilter caps=\"%3\"")
        .arg(QString::number(index), stream.serial,
             QString::fromLatin1(kMixCaps));
}

QString mixedSourceDescription(const QList<Stream> &streams)
{
    QStringList chains;
    chains << QStringLiteral("audiomixer name=%1").arg(mixerElementName());
    // THE SILENCE FLOOR, and it is not decoration. `audiomixer` is an
    // aggregator: with no sink pad that keeps producing, a share started
    // before anything plays — or one whose last application has gone away —
    // hands the encoder nothing, and a published track that carries no
    // samples is a worse outcome than the echo this replaces. Silence sums to
    // nothing, so it costs the far end exactly zero.
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
    // The mixer's own output goes LAST so the caller's `! queue ! …` continues
    // this chain and not one of the sources above it. gst_parse takes the
    // last-written chain as the current one.
    chains << QStringLiteral("%1.").arg(mixerElementName());
    return chains.join(QLatin1Char('\n'));
}

SourceMonitor::~SourceMonitor()
{
    stop();
}

bool perApplicationCaptureAvailable()
{
    // Cached, because the UI asks from a binding and the answer cannot change
    // without the process restarting: it is a question about which plugins
    // this build loaded and whether a PipeWire daemon is reachable.
    static const bool available = [] {
        SourceMonitor probe;
        const bool ok = probe.start();
        probe.stop();
        return ok;
    }();
    return available;
}

#ifdef HAVE_LIGHTNING_WEBRTC

namespace {
/// Does `pipewiresrc` carry the property the retirement path depends on?
///
/// Same discipline as the loopback-element probe in SfuMediaEngine: the
/// element AND the property, because which plugin version a package ships is
/// a packaging fact this code cannot see, and a description naming a property
/// that does not exist fails to PARSE — taking the whole share down rather
/// than degrading.
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
        // STRINGS ARE READ AS STRINGS. `gst_value_serialize()` produces the
        // GStreamer LITERAL, which quotes and backslash-escapes anything
        // containing a space — so `application.name` of `Google Chrome` came
        // back as `"Google Chrome"`, quotes included. That is cosmetic in a
        // log line and NOT cosmetic in the name comparison that excludes our
        // own playback, which compares against an unquoted application name.
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
    // Ask GStreamer only after it exists. `gst_element_factory_find` answers a
    // confident "no such element" when the registry has never been loaded,
    // which would make this report "unsupported" on a machine that supports
    // it perfectly well.
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
    // NO FILTER, AND THAT IS NOT LAZINESS. `gst_device_monitor_add_filter`
    // matches a PROVIDER by the classes it advertises, not a device by its
    // `media.class`: the PipeWire provider advertises Audio/Source,
    // Audio/Sink and Video/Source, so a filter of "Stream/Output/Audio"
    // matches no provider at all and `gst_device_monitor_start()` then
    // returns FALSE. Measured on a live PipeWire 1.6.6 desktop with the
    // provider, `audiomixer` and `on-disconnect` all present: the filtered
    // monitor refused to start and every share silently took the old
    // sink-monitor path — the feature was inert on the machine it was
    // written for, and nothing said so.
    //
    // A caps filter would be wrong for a second reason: a stream node's caps
    // are whatever its application negotiated, so filtering on them would
    // drop the ones we most want. The selection is `streamIsForeign()`,
    // which reads `media.class` off each device.
    gst_device_monitor_add_filter(monitor, nullptr, nullptr);
    if (!gst_device_monitor_start(monitor)) {
        qCInfo(lcShareAudio) << "per-application share audio unavailable: the "
                                "device monitor would not start";
        gst_object_unref(monitor);
        return false;
    }
    // FLUSH THE BUS. `gst_device_monitor_start()` posts DEVICE_ADDED and
    // DEVICE_REMOVED for every play and pause on the desktop, each message
    // holding a ref on a GstDevice and its caps. Nothing here reads the bus —
    // `streams()` asks the monitor for its current list — so without this a
    // long share on a busy machine accumulates messages nobody will ever pop.
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
                                     const QString &ourClientName) const
{
    QList<Stream> out;
    if (!m_monitor)
        return out;
    GList *devices =
        gst_device_monitor_get_devices(static_cast<GstDeviceMonitor *>(m_monitor));
    for (GList *l = devices; l; l = l->next) {
        GstDevice *device = GST_DEVICE(l->data);
        const QVariantMap props = propertiesOf(device);
        if (!streamIsForeign(props, ourPid, ourClientName))
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

QList<Stream> SourceMonitor::streams(qint64, const QString &) const
{
    return {};
}

#endif // HAVE_LIGHTNING_WEBRTC

} // namespace lightning::shareaudio
