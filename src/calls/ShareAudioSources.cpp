#include "calls/ShareAudioSources.h"
#include "calls/RtpVp8Payloader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifdef HAVE_LIGHTNING_WEBRTC
#include "calls/GstBootstrap.h"
#include <gst/gst.h>
#endif

#if defined(Q_OS_WIN) && defined(HAVE_LIGHTNING_WEBRTC)
#include "calls/WindowCaptureSrc.h"
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#endif

namespace {
Q_LOGGING_CATEGORY(lcShareAudio, "lightning.calls.shareaudio")

constexpr const char *kMixCaps = "audio/x-raw,rate=48000,channels=2";

QString propString(const QVariantMap &props, const char *key)
{
    return props.value(QString::fromLatin1(key)).toString().trimmed();
}

std::atomic<int> g_probeState{0};

struct ProbeCache {
    std::mutex mutex;
    bool have = false;
    lightning::shareaudio::ProbeOutcome result =
        lightning::shareaudio::ProbeOutcome::Unknown;
    bool inFlight = false;
    std::chrono::steady_clock::time_point at;
    std::function<lightning::shareaudio::ProbeOutcome()> hook;
    int timeoutMs = 2500;
};

ProbeCache &probeCache()
{
    static ProbeCache cache;
    return cache;
}
}

namespace lightning::shareaudio {

Mode modeFromInt(int value)
{
    switch (value) {
    case static_cast<int>(Mode::System):
        return Mode::System;
    case static_cast<int>(Mode::Apps):
        return Mode::Apps;
    default:
        return Mode::Off;
    }
}

QString Stream::id() const
{
    if (serial.isEmpty())
        return {};
    return startTime != 0
        ? serial + QLatin1Char('@') + QString::number(startTime)
        : serial;
}

QString applicationKeyFor(const QString &appName, const QString &binary,
                          const QString &nodeName)
{
    for (const QString &candidate : { appName, binary, nodeName }) {
        const QString key = candidate.trimmed().toLower();
        if (!key.isEmpty())
            return key;
    }
    return {};
}

QString applicationLabel(const Stream &stream)
{
    for (const QString &candidate :
         { stream.appName, stream.binary, stream.nodeName }) {
        const QString label = candidate.trimmed();
        if (!label.isEmpty())
            return label;
    }
    return {};
}

QString sanitizedIconName(const QString &name)
{
    static const QRegularExpression plain(
        QStringLiteral("\\A[A-Za-z0-9._-]{1,64}\\z"));
    return plain.match(name).hasMatch() ? name : QString();
}

bool Selection::wantsKey(const QString &appKey) const
{
    switch (mode) {
    case Mode::Off:
        return false;
    case Mode::System:
        return true;
    case Mode::Apps:
        return !appKey.isEmpty() && keys.contains(appKey, Qt::CaseInsensitive);
    }
    return false;
}

bool Selection::wants(const Stream &stream) const
{
    if (stream.containsUs)
        return false;
    return wantsKey(stream.appKey);
}

bool Selection::capturesAnything() const
{
    return mode == Mode::System || (mode == Mode::Apps && !keys.isEmpty());
}

bool Selection::operator==(const Selection &other) const
{
    if (mode != other.mode)
        return false;
    if (mode != Mode::Apps)
        return true;
    QStringList a = keys;
    QStringList b = other.keys;
    a.sort(Qt::CaseInsensitive);
    b.sort(Qt::CaseInsensitive);
    return a == b;
}

QList<Application> groupByApplication(const QList<Stream> &streams)
{
    QList<Application> out;
    QHash<QString, int> at;
    for (const Stream &s : streams) {
        if (s.appKey.isEmpty())
            continue;
        const auto known = at.constFind(s.appKey);
        if (known == at.cend()) {
            Application app;
            app.key = s.appKey;
            app.label = applicationLabel(s);
            app.iconName = s.iconName;
            app.streams = 1;
            app.active = s.active;
            app.containsUs = s.containsUs;
            at.insert(s.appKey, out.size());
            out.append(app);
            continue;
        }
        Application &app = out[*known];
        ++app.streams;
        app.active = app.active || s.active;
        app.containsUs = app.containsUs || s.containsUs;
        if (app.iconName.isEmpty())
            app.iconName = s.iconName;
    }
    return out;
}

QString mixerElementName()
{
    return QStringLiteral("shareaudiomix");
}

QString levelElementName()
{
    return QStringLiteral("sharelevel");
}

SystemExclusion systemExclusionFor(Platform platform,
                                   const ExclusionElements &elements)
{
    SystemExclusion verdict;
    switch (platform) {
    case Platform::Linux:
        if (!elements.audiomixer)
            verdict.missing << QStringLiteral("element audiomixer");
        if (!elements.pipewireTargetObject)
            verdict.missing << QStringLiteral(
                "pipewiresrc with target-object (PipeWire 0.3.64 or newer)");
        if (!elements.pipewireDeviceProvider)
            verdict.missing << QStringLiteral("PipeWire device provider");
        verdict.how = QStringLiteral("per-application capture");
        break;
    case Platform::Windows:
        if (!elements.wasapiProcessLoopback)
            verdict.missing << QStringLiteral(
                "wasapi2src with process loopback (Windows build 20348 or "
                "newer)");
        verdict.how = QStringLiteral("process loopback");
        break;
    case Platform::MacOS:
        if (!elements.screenCaptureKit)
            verdict.missing << QStringLiteral("ScreenCaptureKit audio source");
        verdict.how = QStringLiteral("ScreenCaptureKit excluding current-process audio");
        break;
    case Platform::Other:
        verdict.missing << QStringLiteral("a capture of what this computer "
                                          "plays");
        break;
    }
    verdict.possible = verdict.missing.isEmpty();
    if (!verdict.possible)
        verdict.how.clear();
    return verdict;
}

QString systemExclusionStatusLine(const SystemExclusion &verdict)
{
    if (verdict.possible) {
        return QStringLiteral("screen-share sound exclusion capability: available "
                              "(%1; %2)")
            .arg(verdict.how, verdict.how.startsWith(QLatin1String("ScreenCaptureKit"))
                ? QStringLiteral("recording permission required")
                : QStringLiteral("audio server required"));
    }
    return QStringLiteral("screen-share sound exclusion capability: unavailable "
                          "(share audio disabled; missing: %1)")
        .arg(verdict.missing.join(QStringLiteral("; ")));
}

SystemCapture systemCaptureFor(Platform platform, bool perApplicationAvailable,
                               bool processLoopbackAvailable, qint64 ourPid)
{
    if (platform == Platform::Linux && perApplicationAvailable)
        return { SystemCapture::Kind::PerApplication, {}, true };
    if (platform == Platform::MacOS && processLoopbackAvailable)
        return { SystemCapture::Kind::ProcessLoopback,
                 QStringLiteral("lightningmacsystemaudiosrc name=sharesrc"), true };
    if (platform == Platform::Windows && processLoopbackAvailable
        && ourPid > 0 && quint64(ourPid) <= 0xffffffffULL) {
        return { SystemCapture::Kind::ProcessLoopback,
                 QStringLiteral("wasapi2src name=sharesrc "
                                "loopback-mode=exclude-process-tree "
                                "loopback-target-pid=%1 low-latency=true")
                     .arg(ourPid), true };
    }
    return {};
}

QString branchVolumeName(int index)
{
    return QStringLiteral("shareappvol%1").arg(index);
}

QString branchSourceName(int index)
{
    return QStringLiteral("shareapp%1").arg(index);
}

QString branchBinName(int index)
{
    return QStringLiteral("shareappbin%1").arg(index);
}

int branchIndexFromElementName(const QString &name)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\Ashareapp(?:bin|vol)?([0-9]{1,6})\\z"));
    const QRegularExpressionMatch m = pattern.match(name);
    return m.hasMatch() ? m.captured(1).toInt() : -1;
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
    if (serialIsTargetable(serial))
        s.serial = serial;
    s.appName = propString(props, "application.name");
    s.nodeName = propString(props, "node.name");
    s.binary = propString(props, "application.process.binary");
    s.iconName = sanitizedIconName(propString(props, "application.icon-name"));
    s.appKey = applicationKeyFor(s.appName, s.binary, s.nodeName);
    bool ok = false;
    const qint64 pid = propString(props, "application.process.id").toLongLong(&ok);
    s.pid = ok ? pid : -1;
    return s;
}

bool streamIsForeign(const QVariantMap &props, qint64 ourPid,
                     const QStringList &ourNames)
{
    if (propString(props, "media.class") != QLatin1String("Stream/Output/Audio"))
        return false;

    const Stream s = streamFromProperties(props);
    // No serial, nothing to target: pipewiresrc resolves stream nodes only by
    // serial, and anything else captures silence.
    if (s.serial.isEmpty())
        return false;

    if (ourPid > 0 && s.pid == ourPid)
        return false;
    for (const QString &name : ourNames) {
        if (name.isEmpty())
            continue;
        if (s.appName.compare(name, Qt::CaseInsensitive) == 0
            || s.nodeName.compare(name, Qt::CaseInsensitive) == 0
            || s.binary.compare(name, Qt::CaseInsensitive) == 0)
            return false;
    }

    if (!propString(props, "node.link-group").isEmpty())
        return false;

    return true;
}

QString applicationBranchDescription(const Stream &stream, int index,
                                     const BranchOptions &options, bool muted)
{
    if (!serialIsTargetable(stream.serial))
        return QString();
    QString source;
    switch (options.capture) {
    case Capture::WasapiProcess:
        source = QStringLiteral(
                     "wasapi2src name=%1 "
                     "loopback-mode=include-process-tree "
                     "loopback-target-pid=%2 low-latency=true")
                     .arg(branchSourceName(index), stream.serial);
        break;
    case Capture::Test:
        source = QStringLiteral(
                     "audiotestsrc name=%1 is-live=true wave=sine freq=%2 "
                     "volume=0.2")
                     .arg(branchSourceName(index),
                          QString::number(200 + (stream.serial.right(3)
                                                     .toInt() % 50) * 20));
        break;
    case Capture::PipeWire:
        // `min-buffers=1` is pinned: the default changed from 8 to 1 between
        // gst-plugin-pipewire 1.4 and 1.6, and 8 fails to negotiate with
        // sources offering fewer. `on-disconnect=eos` (1.6 and newer) retires
        // the branch when its application quits.
        //
        // `node.dont-fallback` / `dont-reconnect` / `dont-move`: the capture
        // is linked to its target or to NOTHING. Without them the session
        // manager links a capture whose target is missing (a serial gone
        // after a daemon restart, or an application that just quit) to the
        // DEFAULT SOURCE — measured on WirePlumber 0.5.14: target-object=99999
        // was linked to the microphone. That put the user's microphone into
        // the share, and in the Flatpak test read as an application "In the
        // share" that carried silence for minutes. With them, a missing
        // target fails at once ("defined target not found") and the branch is
        // retried against a fresh listing.
        source = QStringLiteral(
                     "pipewiresrc name=%1 target-object=%2 "
                     "min-buffers=1 do-timestamp=true "
                     "stream-properties=\"props,node.dont-fallback=true,"
                     "node.dont-reconnect=true,node.dont-move=true\"%3")
                     .arg(branchSourceName(index), stream.serial,
                          options.retireOnDisconnect
                              ? QStringLiteral(" on-disconnect=eos")
                              : QString());
        break;
    }
    return QStringLiteral(
               "%1 ! queue max-size-time=200000000 leaky=downstream "
               "! audioconvert ! audioresample ! capsfilter caps=\"%2\" "
               "! volume name=%3 mute=%4")
        .arg(source, QString::fromLatin1(kMixCaps), branchVolumeName(index),
             muted ? QStringLiteral("true") : QStringLiteral("false"));
}

QString mixedSourceDescription(const QList<Stream> &streams,
                               const BranchOptions &options)
{
    QStringList chains;
    chains << QStringLiteral("audiomixer name=%1").arg(mixerElementName());
    chains << QStringLiteral("audiotestsrc is-live=true wave=silence "
                             "! %1 ! %2.")
                  .arg(QString::fromLatin1(kMixCaps), mixerElementName());
    int index = 0;
    for (const Stream &s : streams) {
        const QString branch = applicationBranchDescription(s, index, options);
        if (branch.isEmpty())
            continue;
        chains << QStringLiteral("%1 ! %2.").arg(branch, mixerElementName());
        ++index;
    }
    chains << QStringLiteral("%1.").arg(mixerElementName());
    return chains.join(QLatin1Char('\n'));
}

QString shareValveElementName()
{
    return QStringLiteral("sharevalve");
}

QString encodedTrackDescription(const QString &sourceDescription, quint32 ssrc,
                                bool withLevel, bool muted)
{
    return QStringLiteral(
               "%1 ! queue max-size-buffers=0 max-size-bytes=0 "
               "max-size-time=100000000 leaky=downstream "
               "! audioconvert ! audioresample "
               "! audio/x-raw,channels=2,rate=48000 "
               "%3"
               "! valve name=%5 drop=%6 "
               "! opusenc name=shareaudioenc audio-type=generic "
               "bitrate=128000 "
               "! rtpopuspay pt=111 ssrc=%2 mtu=%4 "
               "! capsfilter caps=\"application/x-rtp,media=audio,"
               "encoding-name=OPUS,payload=111,clock-rate=(int)48000,"
               "encoding-params=(string)2,ssrc=(uint)%2\"")
        .arg(sourceDescription, QString::number(ssrc),
             withLevel ? QStringLiteral("! level name=%1 "
                                        "interval=1000000000 "
                                        "post-messages=true ")
                             .arg(levelElementName())
                       : QString(),
             QString::number(lightning::rtp::kRtpPayloadMtu),
             shareValveElementName(),
             muted ? QStringLiteral("true") : QStringLiteral("false"));
}

QSet<qint64> ancestorsOf(qint64 pid, const QHash<qint64, qint64> &parentOf)
{
    QSet<qint64> out;
    qint64 at = pid;
    for (int depth = 0; depth < 64; ++depth) {
        const auto parent = parentOf.constFind(at);
        if (parent == parentOf.cend() || *parent <= 0 || *parent == at
            || *parent == pid || out.contains(*parent))
            break;
        at = *parent;
        out.insert(at);
    }
    return out;
}

QList<qint64> dropDescendants(const QList<qint64> &pids,
                              const QHash<qint64, qint64> &parentOf)
{
    const QSet<qint64> listed(pids.cbegin(), pids.cend());
    QList<qint64> out;
    for (qint64 pid : pids) {
        if (out.contains(pid))
            continue;
        bool covered = false;
        for (qint64 ancestor : ancestorsOf(pid, parentOf)) {
            if (listed.contains(ancestor)) {
                covered = true;
                break;
            }
        }
        if (!covered)
            out.append(pid);
    }
    return out;
}

void markProcessesContainingUs(QList<Stream> &streams,
                               const QHash<qint64, qint64> &parentOf,
                               qint64 ourPid)
{
    if (ourPid <= 0)
        return;
    const QSet<qint64> ours = ancestorsOf(ourPid, parentOf);
    for (Stream &s : streams) {
        if (s.pid > 0 && ours.contains(s.pid))
            s.containsUs = true;
    }
}

ScanPlan planScan(const ScanInput &in)
{
    ScanPlan plan;
    QHash<QString, Stream> live;
    for (const Stream &s : in.live) {
        const QString id = s.id();
        if (!id.isEmpty() && !live.contains(id))
            live.insert(id, s);
    }

    QSet<QString> retiring;
    for (auto it = in.records.cbegin(); it != in.records.cend(); ++it) {
        const BranchRecord &r = it.value();
        if (r.index < 0 || r.retired || r.failed)
            continue;
        const auto now = live.constFind(it.key());
        if (now != live.cend()) {
            if (r.appKey.isEmpty() || now->appKey.isEmpty()
                || r.appKey.compare(now->appKey, Qt::CaseInsensitive) == 0)
                continue;
        } else if (in.options.followsProcess && in.running.contains(it.key())) {
            continue;
        }
        plan.retire << it.key();
        retiring.insert(it.key());
    }

    struct Candidate {
        QString id;
        qint64 pid = -1;
        QString key;
        bool built = false;
        bool muted = false;
        bool wanted = false;
        Stream stream;
    };
    QList<Candidate> candidates;
    for (auto it = in.records.cbegin(); it != in.records.cend(); ++it) {
        const BranchRecord &r = it.value();
        if (r.index < 0 || r.retired || r.failed || retiring.contains(it.key()))
            continue;
        Candidate c;
        c.id = it.key();
        c.pid = r.pid;
        c.key = r.appKey;
        c.built = true;
        c.muted = r.muted;
        c.wanted = in.selection.wantsKey(r.appKey);
        candidates << c;
    }
    for (const Stream &s : in.live) {
        const QString id = s.id();
        if (id.isEmpty() || in.records.contains(id))
            continue;
        if (!in.selection.wants(s))
            continue;
        Candidate c;
        c.id = id;
        c.pid = s.pid;
        c.key = s.appKey;
        c.wanted = true;
        c.stream = s;
        candidates << c;
    }

    QSet<QString> excluded;
    QSet<qint64> outermost;
    const auto computeOutermost = [&] {
        outermost.clear();
        if (!in.options.followsProcess)
            return;
        QList<qint64> pids;
        for (const Candidate &c : candidates) {
            if (c.wanted && c.pid > 0 && !excluded.contains(c.id)
                && !pids.contains(c.pid))
                pids << c.pid;
        }
        const QList<qint64> kept = dropDescendants(pids, in.parents);
        outermost = QSet<qint64>(kept.cbegin(), kept.cend());
    };
    const auto desired = [&](const Candidate &c) {
        if (!c.wanted || excluded.contains(c.id))
            return false;
        if (!in.options.followsProcess)
            return true;
        return c.pid <= 0 || outermost.contains(c.pid);
    };
    for (int round = 0; round < 8; ++round) {
        computeOutermost();
        int added = 0;
        bool changed = false;
        for (const Candidate &c : candidates) {
            if (c.built || !desired(c))
                continue;
            if (in.liveBranches + added >= in.cap) {
                excluded.insert(c.id);
                changed = true;
                continue;
            }
            ++added;
        }
        if (!changed)
            break;
        plan.limitReached = true;
    }
    computeOutermost();

    QSet<QString> carried;
    for (const Candidate &c : candidates) {
        const bool audible = desired(c);
        if (c.built) {
            if (c.muted == audible)
                plan.mute << qMakePair(c.id, !audible);
            if (audible && !c.key.isEmpty())
                carried.insert(c.key);
            continue;
        }
        if (!audible)
            continue;
        plan.add << c.stream;
        if (!c.key.isEmpty())
            carried.insert(c.key);
    }
    plan.carried = QStringList(carried.cbegin(), carried.cend());
    plan.carried.sort();
    return plan;
}

int privatePipeWireConnection()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    QByteArray name = qgetenv("PIPEWIRE_REMOTE");
    if (const int comma = name.indexOf(','); comma >= 0)
        name.truncate(comma);
    name = name.trimmed();
    if (name.isEmpty())
        name = QByteArrayLiteral("pipewire-0");
    QByteArray path;
    if (name.startsWith('/')) {
        path = name;
    } else {
        QByteArray dir = qgetenv("PIPEWIRE_RUNTIME_DIR");
        if (dir.isEmpty())
            dir = qgetenv("XDG_RUNTIME_DIR");
        if (dir.isEmpty())
            return -1;
        path = dir + '/' + name;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= static_cast<int>(sizeof(address.sun_path)))
        return -1;
    std::memcpy(address.sun_path, path.constData(),
                static_cast<size_t>(path.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address))
        != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
#else
    return -1;
#endif
}

bool connectionClosed(int fd)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    if (fd < 0)
        return false;
    pollfd entry{};
    entry.fd = fd;
    entry.events = POLLRDHUP;
    if (::poll(&entry, 1, 0) <= 0)
        return false;
    return (entry.revents & (POLLHUP | POLLERR | POLLRDHUP | POLLNVAL)) != 0;
#else
    Q_UNUSED(fd);
    return false;
#endif
}

#ifdef HAVE_LIGHTNING_WEBRTC
namespace {
void closeFdWhenFinalized(gpointer data, GObject *)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    ::close(GPOINTER_TO_INT(data));
#else
    Q_UNUSED(data);
#endif
}
}

int givePrivatePipeWireConnection(GObject *object)
{
    if (!object
        || !g_object_class_find_property(G_OBJECT_GET_CLASS(object), "fd"))
        return -1;
    const int fd = privatePipeWireConnection();
    if (fd < 0)
        return -1;
    g_object_set(object, "fd", fd, nullptr);
    g_object_weak_ref(object, closeFdWhenFinalized, GINT_TO_POINTER(fd));
    return fd;
}
#else
int givePrivatePipeWireConnection(::_GObject *)
{
    return -1;
}
#endif

#ifdef HAVE_LIGHTNING_WEBRTC

namespace {

bool elementExists(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

bool elementHasProperty(const char *element, const char *property)
{
    GstElementFactory *factory = gst_element_factory_find(element);
    if (!factory)
        return false;
    GstElement *probe = gst_element_factory_create(factory, nullptr);
    gst_object_unref(factory);
    if (!probe)
        return false;
    const bool has = g_object_class_find_property(G_OBJECT_GET_CLASS(probe),
                                                  property) != nullptr;
    gst_object_unref(probe);
    return has;
}

ExclusionElements loadedExclusionElements()
{
    ExclusionElements e;
    e.audiomixer = elementExists("audiomixer");
#if defined(Q_OS_MACOS)
    e.screenCaptureKit = elementExists("lightningmacsystemaudiosrc");
#elif defined(Q_OS_WIN)
    e.wasapiSource = elementExists("wasapi2src");
    e.wasapiProcessLoopback =
        elementHasProperty("wasapi2src", "loopback-target-pid")
        && elementHasProperty("wasapi2src", "loopback-mode");
#else
    e.pipewireTargetObject = elementHasProperty("pipewiresrc", "target-object");
    if (GstDeviceProviderFactory *provider =
            gst_device_provider_factory_find("pipewiredeviceprovider")) {
        e.pipewireDeviceProvider = true;
        gst_object_unref(provider);
    }
#endif
    return e;
}

#if !defined(Q_OS_WIN)
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

std::mutex g_pipewirePassLock;

QList<QVariantMap> pipewireDevicePassLocked(bool *reached)
{
    QList<QVariantMap> out;
    if (reached)
        *reached = false;
    const std::lock_guard<std::mutex> serialized(g_pipewirePassLock);
    GstDeviceProvider *provider =
        gst_device_provider_factory_get_by_name("pipewiredeviceprovider");
    if (!provider)
        return out;
    const bool hasFd =
        g_object_class_find_property(G_OBJECT_GET_CLASS(provider), "fd")
        != nullptr;
    const int fd = hasFd ? privatePipeWireConnection() : -1;
    if (fd >= 0)
        g_object_set(provider, "fd", fd, nullptr);
    if (gst_device_provider_start(provider)) {
        GList *devices = gst_device_provider_get_devices(provider);
        if (reached)
            *reached = devices != nullptr;
        for (GList *l = devices; l; l = l->next)
            out.append(propertiesOf(GST_DEVICE(l->data)));
        g_list_free_full(devices, gst_object_unref);
        gst_device_provider_stop(provider);
    }
    if (fd >= 0) {
        g_object_set(provider, "fd", -1, nullptr);
        ::close(fd);
    }
    gst_object_unref(provider);
    return out;
}

ProbeOutcome probePipeWire()
{
    const SystemExclusion elements =
        systemExclusionFor(Platform::Linux, loadedExclusionElements());
    if (!elements.possible) {
        qCInfo(lcShareAudio).noquote()
            << "per-application share audio unavailable: missing"
            << elements.missing.join(QStringLiteral(", "));
        return ProbeOutcome::Unavailable;
    }
    // start() answers TRUE when it could not connect at all ("failed: return
    // TRUE" in every version through 1.6), so a PulseAudio-only system looked
    // capable and every share carried the silence floor alone. Reaching the
    // daemon is proved by the provider having seen at least one node.
    bool reached = false;
    pipewireDevicePassLocked(&reached);
    if (!reached) {
        qCInfo(lcShareAudio)
            << "per-application share audio unavailable: no PipeWire daemon "
               "answered (a PulseAudio-only system, or PipeWire not running "
               "yet); asking again when share options open";
        return ProbeOutcome::Unknown;
    }
    return ProbeOutcome::Available;
}
#endif

#if defined(Q_OS_WIN)

const CLSID kMMDeviceEnumeratorClsid = {
    0xbcde0395, 0xe52f, 0x467c,
    { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e }
};

template <typename T>
struct ComRef {
    T *p = nullptr;
    ComRef() = default;
    ComRef(const ComRef &) = delete;
    ComRef &operator=(const ComRef &) = delete;
    ~ComRef()
    {
        if (p)
            p->Release();
    }
    T *operator->() const { return p; }
    T **out() { return &p; }
};

bool inProcessTree(qint64 pid, qint64 root,
                   const QHash<qint64, qint64> &parents)
{
    return pid == root || ancestorsOf(pid, parents).contains(root);
}

QList<Stream> windowsSessions(qint64 ourPid,
                              const QHash<qint64, qint64> &parents)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsInit = SUCCEEDED(init);
    QHash<qint64, bool> activeByPid;
    QList<qint64> order;
    {
        ComRef<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED(CoCreateInstance(kMMDeviceEnumeratorClsid, nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(enumerator.out())))) {
            ComRef<IMMDeviceCollection> devices;
            if (SUCCEEDED(enumerator->EnumAudioEndpoints(
                    eRender, DEVICE_STATE_ACTIVE, devices.out()))) {
                UINT deviceCount = 0;
                devices->GetCount(&deviceCount);
                for (UINT d = 0; d < deviceCount; ++d) {
                    ComRef<IMMDevice> device;
                    if (FAILED(devices->Item(d, device.out())))
                        continue;
                    ComRef<IAudioSessionManager2> manager;
                    if (FAILED(device->Activate(
                            __uuidof(IAudioSessionManager2),
                            CLSCTX_INPROC_SERVER, nullptr,
                            reinterpret_cast<void **>(manager.out()))))
                        continue;
                    ComRef<IAudioSessionEnumerator> sessions;
                    if (FAILED(manager->GetSessionEnumerator(sessions.out())))
                        continue;
                    int count = 0;
                    sessions->GetCount(&count);
                    for (int i = 0; i < count; ++i) {
                        ComRef<IAudioSessionControl> control;
                        if (FAILED(sessions->GetSession(i, control.out())))
                            continue;
                        ComRef<IAudioSessionControl2> control2;
                        if (FAILED(control->QueryInterface(
                                IID_PPV_ARGS(control2.out()))))
                            continue;
                        if (control2->IsSystemSoundsSession() == S_OK)
                            continue;
                        AudioSessionState state = AudioSessionStateInactive;
                        control->GetState(&state);
                        if (state == AudioSessionStateExpired)
                            continue;
                        DWORD pid = 0;
                        if (FAILED(control2->GetProcessId(&pid)) || pid == 0)
                            continue;
                        const qint64 key = static_cast<qint64>(pid);
                        const bool active = state == AudioSessionStateActive;
                        if (!activeByPid.contains(key))
                            order.append(key);
                        activeByPid[key] = activeByPid.value(key) || active;
                    }
                }
            }
        }
    }
    if (ownsInit)
        CoUninitialize();

    QList<Stream> out;
    for (qint64 pid : order) {
        if (ourPid > 0 && inProcessTree(pid, ourPid, parents))
            continue;
        const lightning::wincap::ProcessInfo info =
            lightning::wincap::processInfo(static_cast<quint64>(pid));
        if (!info.running)
            continue;
        Stream s;
        s.serial = QString::number(pid);
        s.pid = pid;
        s.startTime = info.startTime;
        s.binary =
            info.executablePath.section(QLatin1Char('\\'), -1).toLower();
        s.appName = lightning::wincap::applicationNameForExecutablePath(
            info.executablePath);
        s.nodeName = s.binary;
        s.appKey = applicationKeyFor(s.binary, s.appName, QString());
        s.active = activeByPid.value(pid);
        if (s.appKey.isEmpty())
            continue;
        out.append(s);
    }
    markProcessesContainingUs(out, parents, ourPid);
    return out;
}

#endif

}

#endif

SystemExclusion systemExclusion()
{
#if defined(HAVE_LIGHTNING_WEBRTC) && (defined(Q_OS_WIN) || defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
    if (!lightning::gst::ensureInitialised())
        return systemExclusionFor(Platform::Other, {});
#if defined(Q_OS_MACOS)
    return systemExclusionFor(Platform::MacOS, loadedExclusionElements());
#elif defined(Q_OS_WIN)
    return systemExclusionFor(Platform::Windows, loadedExclusionElements());
#else
    return systemExclusionFor(Platform::Linux, loadedExclusionElements());
#endif
#else
    return systemExclusionFor(Platform::Other, {});
#endif
}

SystemCapture systemCapture(qint64 ourPid)
{
#if defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_MACOS)
    return systemCaptureFor(Platform::MacOS, false, systemExclusion().possible, ourPid);
#elif defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_WIN)
    static const bool available = systemExclusion().possible;
    return systemCaptureFor(Platform::Windows, false, available, ourPid);
#elif defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_LINUX)
    return systemCaptureFor(Platform::Linux,
                            perApplicationCaptureAvailable(), false, ourPid);
#else
    Q_UNUSED(ourPid);
    return {};
#endif
}

Unavailability unavailabilityFor(Platform platform,
                                 const ExclusionElements &elements,
                                 bool daemonReached)
{
    switch (platform) {
    case Platform::Linux:
        if (!elements.audiomixer || !elements.pipewireTargetObject
            || !elements.pipewireDeviceProvider) {
            return Unavailability::MissingPlugin;
        }
        return daemonReached ? Unavailability::None : Unavailability::NoPipeWire;
    case Platform::Windows:
        if (!elements.wasapiSource)
            return Unavailability::MissingPlugin;
        return elements.wasapiProcessLoopback ? Unavailability::None
                                              : Unavailability::OldWindows;
    case Platform::MacOS:
        return elements.screenCaptureKit ? Unavailability::None : Unavailability::MissingPlugin;
    case Platform::Other:
        break;
    }
    return Unavailability::NoCapture;
}

Unavailability systemUnavailability()
{
#if defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_MACOS)
    return systemExclusion().possible ? Unavailability::None : Unavailability::MissingPlugin;
#elif defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_WIN)
    if (!lightning::gst::ensureInitialised())
        return Unavailability::NoCapture;
    return unavailabilityFor(Platform::Windows, loadedExclusionElements(), false);
#elif defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_LINUX)
    if (!lightning::gst::ensureInitialised())
        return Unavailability::NoCapture;
    return unavailabilityFor(Platform::Linux, loadedExclusionElements(),
                             perApplicationCaptureAvailable());
#else
    return Unavailability::NoCapture;
#endif
}

QHash<qint64, qint64> processParents()
{
    QHash<qint64, qint64> parents;
#if defined(Q_OS_WIN) && defined(HAVE_LIGHTNING_WEBRTC)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return parents;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            parents.insert(static_cast<qint64>(entry.th32ProcessID),
                           static_cast<qint64>(entry.th32ParentProcessID));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#endif
    return parents;
}

QList<QVariantMap> pipewireDevices(bool *reached)
{
#if defined(HAVE_LIGHTNING_WEBRTC) && !defined(Q_OS_WIN)
    return pipewireDevicePassLocked(reached);
#else
    if (reached)
        *reached = false;
    return {};
#endif
}

Enumeration enumerate(qint64 ourPid, const QStringList &ourNames,
                      const QList<Stream> &checkRunning)
{
    Enumeration result;
    QElapsedTimer clock;
    clock.start();
#ifdef HAVE_LIGHTNING_WEBRTC
    if (g_probeState.load() != 1)
        return result;
#if defined(Q_OS_WIN)
    Q_UNUSED(ourNames);
    result.parents = processParents();
    result.streams = windowsSessions(ourPid, result.parents);
    for (const Stream &c : checkRunning) {
        if (c.pid <= 0)
            continue;
        const lightning::wincap::ProcessInfo info =
            lightning::wincap::processInfo(static_cast<quint64>(c.pid));
        if (info.running && info.startTime == c.startTime)
            result.running.insert(c.id());
    }
#else
    Q_UNUSED(checkRunning);
    for (const QVariantMap &props : pipewireDevicePassLocked(nullptr)) {
        if (!streamIsForeign(props, ourPid, ourNames))
            continue;
        result.streams.append(streamFromProperties(props));
    }
#endif
#else
    Q_UNUSED(ourPid);
    Q_UNUSED(ourNames);
    Q_UNUSED(checkRunning);
#endif
    result.elapsedMs = clock.elapsed();
    return result;
}

void enumerateAsync(QObject *context, qint64 ourPid,
                    const QStringList &ourNames,
                    const QList<Stream> &checkRunning,
                    std::function<void(const Enumeration &)> done)
{
    QPointer<QObject> guard(context);
    std::thread([guard, ourPid, ourNames, checkRunning,
                 done = std::move(done)]() mutable {
        Enumeration result;
        try {
            result = enumerate(ourPid, ourNames, checkRunning);
        } catch (...) {
        }
        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [guard, result, done = std::move(done)]() {
                if (guard)
                    done(result);
            },
            Qt::QueuedConnection);
    }).detach();
}

#ifdef HAVE_LIGHTNING_WEBRTC
static ProbeOutcome runProbe()
{
#if defined(Q_OS_WIN)
    const bool haveMixer = elementExists("audiomixer");
    const bool haveSource = elementExists("wasapi2src");
    const bool haveLoopback = haveSource
        && loadedExclusionElements().wasapiProcessLoopback;
    const bool ok = haveMixer && haveLoopback;
    if (!ok) {
        QStringList missing;
        if (!haveMixer)
            missing << QStringLiteral("element audiomixer");
        if (!haveSource)
            missing << QStringLiteral("element wasapi2src");
        else if (!haveLoopback)
            missing << QStringLiteral(
                "wasapi2src property loopback-target-pid (needs Windows "
                "build 20348 or newer)");
        qCInfo(lcShareAudio).noquote()
            << "per-application share audio unavailable: missing"
            << missing.join(QStringLiteral(", "));
    }
    return ok ? ProbeOutcome::Available : ProbeOutcome::Unavailable;
#else
    return probePipeWire();
#endif
}
#endif

bool perApplicationCaptureAvailable(bool reprobe)
{
#ifndef HAVE_LIGHTNING_WEBRTC
    Q_UNUSED(reprobe);
    return false;
#else
    ProbeCache &cache = probeCache();
    std::function<ProbeOutcome()> body;
    int timeoutMs = 2500;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.have && cache.result != ProbeOutcome::Unknown)
            return cache.result == ProbeOutcome::Available;
        if (cache.inFlight)
            return false;
        if (!reprobe && cache.have
            && std::chrono::steady_clock::now() - cache.at
                   < std::chrono::seconds(5)) {
            return false;
        }
        body = cache.hook;
        timeoutMs = cache.timeoutMs;
    }
    if (!body && !lightning::gst::ensureInitialised())
        return false;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.inFlight)
            return false;
        cache.inFlight = true;
    }
    auto slot = std::make_shared<std::promise<ProbeOutcome>>();
    auto answer = slot->get_future();
    std::thread([slot, body] {
        ProbeOutcome outcome = ProbeOutcome::Unknown;
        try {
            outcome = body ? body() : runProbe();
        } catch (...) {
        }
        {
            ProbeCache &c = probeCache();
            std::lock_guard<std::mutex> lock(c.mutex);
            c.have = true;
            c.result = outcome;
            c.inFlight = false;
            c.at = std::chrono::steady_clock::now();
            g_probeState.store(outcome == ProbeOutcome::Available ? 1 : 2);
        }
        try {
            slot->set_value(outcome);
        } catch (...) {
        }
    }).detach();
    if (answer.wait_for(std::chrono::milliseconds(timeoutMs))
        != std::future_status::ready) {
        qCWarning(lcShareAudio)
            << "the audio session probe did not answer within" << timeoutMs
            << "ms; per-application share audio is unavailable until it does";
        return false;
    }
    const ProbeOutcome outcome = answer.get();
    qCInfo(lcShareAudio) << "per-application share audio available="
                         << (outcome == ProbeOutcome::Available)
                         << (outcome == ProbeOutcome::Unknown
                                 ? "(not final: asked again later)" : "");
    return outcome == ProbeOutcome::Available;
#endif
}

void setPerApplicationProbeForTest(std::function<ProbeOutcome()> probe,
                                   int timeoutMs)
{
    ProbeCache &cache = probeCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.hook = std::move(probe);
    cache.timeoutMs = timeoutMs;
}

void resetPerApplicationProbeForTest()
{
    ProbeCache &cache = probeCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.have = false;
    cache.result = ProbeOutcome::Unknown;
    cache.inFlight = false;
    cache.hook = nullptr;
    cache.timeoutMs = 2500;
    g_probeState.store(0);
}

BranchOptions branchOptions()
{
    BranchOptions options;
#if defined(Q_OS_WIN)
    options.capture = Capture::WasapiProcess;
    options.retireOnDisconnect = false;
    options.followsProcess = true;
#elif defined(HAVE_LIGHTNING_WEBRTC)
    options.capture = Capture::PipeWire;
    static const bool canRetire = [] {
        lightning::gst::ensureInitialised();
        return elementHasProperty("pipewiresrc", "on-disconnect");
    }();
    options.retireOnDisconnect =
        canRetire
        && !qEnvironmentVariableIsSet("LIGHTNING_SHARE_AUDIO_LEGACY_PIPEWIRE");
#endif
    return options;
}

bool SourceMonitor::start()
{
    m_running = perApplicationCaptureAvailable();
    return m_running;
}

QList<Stream> SourceMonitor::streams(qint64 ourPid,
                                     const QStringList &ourNames) const
{
    if (!m_running)
        return {};
    return enumerate(ourPid, ourNames).streams;
}

}
