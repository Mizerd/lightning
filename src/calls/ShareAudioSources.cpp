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
#include <unistd.h> // ::close in the PipeWire pass, also compiled on macOS
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

/// The caps all mixer inputs agree on: stereo 48 kHz, what the share-audio
/// encoder wants, so each branch resamples once.
constexpr const char *kMixCaps = "audio/x-raw,rate=48000,channels=2";

QString propString(const QVariantMap &props, const char *key)
{
    return props.value(QString::fromLatin1(key)).toString().trimmed();
}

/// The bounded probe's answer: 0 not yet known, 1 available, 2 not. Read by
/// enumerate(), which must never be the thing that starts the probe (a
/// worker that hung inside it would hold its once-flag for good).
std::atomic<int> g_probeState{0};

/// The per-application probe's cache. A definite answer stays; Unknown is
/// asked again (perApplicationCaptureAvailable()). All guarded by the mutex.
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
} // namespace

namespace lightning::shareaudio {

Mode modeFromInt(int value)
{
    switch (value) {
    case static_cast<int>(Mode::System):
        return Mode::System;
    case static_cast<int>(Mode::Apps):
        return Mode::Apps;
    default:
        // Off, and anything a newer build wrote: capturing nothing is the
        // only safe reading of a mode this build does not know.
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
    // A process Lightning runs inside: its tree includes our own playback.
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
    // Order is not meaning.
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
        // Exactly what probePipeWire() requires before it asks the daemon:
        // one pipewiresrc per application, found by the PipeWire provider,
        // summed by an audiomixer. A sink monitor cannot leave a contributor
        // out, so all three are required for an excluding capture.
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
        // "The whole system" is one exclude-process-tree loopback; the mixer
        // is only for chosen applications.
        if (!elements.wasapiProcessLoopback)
            verdict.missing << QStringLiteral(
                "wasapi2src with process loopback (Windows build 20348 or "
                "newer)");
        verdict.how = QStringLiteral("process loopback");
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
                              "(%1; audio server required)")
            .arg(verdict.how);
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
    // Digits only; see serialIsTargetable().
    if (serialIsTargetable(serial))
        s.serial = serial;
    s.appName = propString(props, "application.name");
    s.nodeName = propString(props, "node.name");
    s.binary = propString(props, "application.process.binary");
    // Any client sets this; it reaches QIcon::fromTheme in the picker.
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
            || s.nodeName.compare(name, Qt::CaseInsensitive) == 0
            || s.binary.compare(name, Qt::CaseInsensitive) == 0)
            return false;
    }

    // Virtual plumbing (`pw-loopback`, `filter-chain`) sets
    // `node.link-group`; its playback side repeats audio an application
    // already produced.
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
        // Process loopback: this process and its children, wherever they
        // play. The serial is the session's process id.
        source = QStringLiteral(
                     "wasapi2src name=%1 "
                     "loopback-mode=include-process-tree "
                     "loopback-target-pid=%2 low-latency=true")
                     .arg(branchSourceName(index), stream.serial);
        break;
    case Capture::Test:
        // A distinct tone per serial, so a test can tell branches apart.
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
               // An explicit capsfilter, not bare caps: bare caps followed by
               // nothing read as an element name ("no element \"audio\"").
               "! audioconvert ! audioresample ! capsfilter caps=\"%2\" "
               // The branch's own volume: deselecting an application mid-share
               // mutes its branch rather than unlinking a pad that carries
               // data on a live pipeline (the unpublish deadlock).
               "! volume name=%3 mute=%4")
        .arg(source, QString::fromLatin1(kMixCaps), branchVolumeName(index),
             muted ? QStringLiteral("true") : QStringLiteral("false"));
}

QString mixedSourceDescription(const QList<Stream> &streams,
                               const BranchOptions &options)
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
        const QString branch = applicationBranchDescription(s, index, options);
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

QString shareValveElementName()
{
    return QStringLiteral("sharevalve");
}

QString encodedTrackDescription(const QString &sourceDescription, quint32 ssrc,
                                bool withLevel, bool muted)
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
               // What is actually being sent, once a second: a share carrying
               // silence must be distinguishable from one carrying sound.
               "%3"
               // Its own valve, so muting share audio can never touch the
               // microphone (SfuMediaEngine::setShareAudioMuted). After the
               // meter, so a muted share still says whether it hears sound.
               "! valve name=%5 drop=%6 "
               "! opusenc name=shareaudioenc audio-type=generic "
               "bitrate=128000 "
               "! rtpopuspay pt=111 ssrc=%2 mtu=%4 "
               // The ssrc must be in the caps, as for the microphone bin, or
               // the offer has no a=ssrc and the SFU cannot attribute the RTP.
               "! capsfilter caps=\"application/x-rtp,media=audio,"
               "encoding-name=OPUS,payload=111,clock-rate=(int)48000,"
               "encoding-params=(string)2,ssrc=(uint)%2\"")
        // One pass, so nothing substituted is scanned for markers again.
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
    // Bounded walk: a pid-reuse cycle in a stale snapshot must not hang.
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

    // 1. Retire what is gone: a stream node that no longer exists, or (when a
    //    branch follows its process) a process that has exited. A Windows
    //    session that expired while its process runs keeps its branch. A live
    //    id that now names a DIFFERENT application is a reused serial (a
    //    restarted PipeWire numbers its objects again): the old branch is not
    //    that stream, so it goes too, and the new stream is added once the old
    //    record has been taken out.
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

    // 2. Every candidate: built branches still standing, and live wanted
    //    streams with no record (a failed record is never retried while it
    //    stands).
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

    // 3. Which new candidates get a branch, and which candidates are audible.
    //    A chosen process inside another chosen process's tree is already
    //    captured by it (include-process-tree): only among the CHOSEN, never
    //    applied to what is listed, and only among those that actually HAVE a
    //    branch or get one, so an ancestor the cap leaves out does not mute
    //    the descendant that is carrying its sound.
    QSet<QString> excluded;   // new candidates the cap keeps out
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
    // The cap and the tree feed each other; a few rounds settle it (each round
    // only excludes more, and there are finitely many candidates).
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

    // 4. Mute or unmute what stands; 5. build what is new.
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

// ── Private PipeWire connections ──

int privatePipeWireConnection()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    // Where libpipewire's native protocol looks: PIPEWIRE_REMOTE (a name, or
    // an absolute socket path), else "pipewire-0", in PIPEWIRE_RUNTIME_DIR,
    // else XDG_RUNTIME_DIR.
    QByteArray name = qgetenv("PIPEWIRE_REMOTE");
    if (const int comma = name.indexOf(','); comma >= 0)
        name.truncate(comma);   // a list of remotes: the first is the default
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
    // Non-blocking, as libpipewire opens its own: a branch is added on the
    // GUI thread, and a blocking connect() to a stuck daemon whose backlog is
    // full would wait there. Non-blocking, that is EAGAIN, and the caller
    // keeps the shared connection.
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
} // namespace

int givePrivatePipeWireConnection(GObject *object)
{
    if (!object
        || !g_object_class_find_property(G_OBJECT_GET_CLASS(object), "fd"))
        return -1;
    const int fd = privatePipeWireConnection();
    if (fd < 0)
        return -1;
    g_object_set(object, "fd", fd, nullptr);
    // Closed only once the object is gone: by then it has released its core,
    // so the fd number cannot lead a later object into it.
    g_object_weak_ref(object, closeFdWhenFinalized, GINT_TO_POINTER(fd));
    return fd;
}
#else
int givePrivatePipeWireConnection(::_GObject *)
{
    return -1;
}
#endif

// ── Enumeration and the bounded probe ──

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

/// Whether `element` has `property`. Checked because naming a missing
/// property fails the whole parse, and packaged plugin versions vary.
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

/// What the registry offers towards leaving ourselves out of a share.
ExclusionElements loadedExclusionElements()
{
    ExclusionElements e;
    e.audiomixer = elementExists("audiomixer");
#if defined(Q_OS_WIN)
    // The properties are installed only on Windows build 20348+.
    e.wasapiSource = elementExists("wasapi2src");
    e.wasapiProcessLoopback =
        elementHasProperty("wasapi2src", "loopback-target-pid")
        && elementHasProperty("wasapi2src", "loopback-mode");
#else
    // `target-object` arrived in 0.3.6x; 0.3.48 (Ubuntu 22.04, Mint 21) has
    // only `path`, which cannot name a stream by serial. `on-disconnect` is
    // used where present and not required: it exists only from PipeWire 1.6,
    // and requiring it switched per-application capture off in every package
    // and on every distribution older than that.
    e.pipewireTargetObject = elementHasProperty("pipewiresrc", "target-object");
    // The PipeWire provider alone, not an unfiltered GstDeviceMonitor: that
    // started every provider on the machine, and stream nodes are only ever
    // PipeWire's.
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

/// Every pass on the provider, serialized. The PipeWire device provider is a
/// process-wide SINGLETON (gst_device_provider_factory_get_by_name caches it
/// on its factory), shared by the engine's scans, the app list and the probe,
/// which run on different workers: its fd, its start count and the core it
/// joins must never be touched by two passes at once.
std::mutex g_pipewirePassLock;

/// One connection to the PipeWire daemon for one pass: start the provider on
/// a private connection, read its devices, stop it, close the connection.
/// Never kept between passes: a provider stays bound to the daemon it first
/// reached, and after a PipeWire restart a kept one would go on answering
/// with a dead connection (its core error is only recorded) while stale
/// serials meant other nodes. Runs on a worker (enumerateAsync) or in the
/// bounded probe, never on the GUI thread: start() waits on the daemon with no
/// timeout. `reached` is whether any node at all was seen, since start()
/// reports success without a daemon too.
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
    // Our own connection, never the plugin's shared one: that one is dead
    // after a daemon restart for as long as anything still holds it, and a
    // start() on it waits for ever (see privatePipeWireConnection()). Owned
    // HERE, not by a weak reference: the singleton is never finalized, so a
    // weak-ref close never ran and every pass leaked a connected socket and a
    // client on the daemon (the plugin dups the fd; ours kept it open).
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
        // Releases the core it joined (the last reference: nothing else uses
        // our fd), which disconnects the plugin's dup of the socket.
        gst_device_provider_stop(provider);
    }
    if (fd >= 0) {
        // The core is gone, so the number may be reused: the provider must
        // not name it any more.
        g_object_set(provider, "fd", -1, nullptr);
        ::close(fd);
    }
    gst_object_unref(provider);
    return out;
}

/// The probe's work on Linux: checks, then one pass that reached a daemon.
/// Missing plugins are definite; no daemon answering is not (it may be
/// starting, or this may be PulseAudio alone, and nothing here can tell).
ProbeOutcome probePipeWire()
{
    // The element half is the very verdict `--call-media-status` reports
    // (systemExclusionFor()), so the package check and this gate cannot
    // disagree about what is required.
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
#endif // !Q_OS_WIN

#if defined(Q_OS_WIN)

// CLSID_MMDeviceEnumerator written out, as SpellBackend.cpp does for its
// factory: DEFINE_GUID only declares the symbol without INITGUID. Interface
// ids come from the headers through IID_PPV_ARGS / __uuidof.
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

/// The audio sessions on every active render endpoint, one per process.
/// Runs on a worker (enumerateAsync), so COM is initialised here.
QList<Stream> windowsSessions(qint64 ourPid,
                              const QHash<qint64, qint64> &parents)
{
    // Multithreaded on a worker; S_OK and S_FALSE need a matching
    // CoUninitialize, RPC_E_CHANGED_MODE does not.
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
                        // The system-sounds session belongs to no application.
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
        // Never our own process or a child of it: our playback is the echo.
        // (An ANCESTOR is listed, marked containsUs, and never captured.)
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
        // Keyed by the executable on Windows, so a window (whose owning
        // process is often not the one holding the audio session, as in
        // every Chromium browser) maps to the same key as its sound.
        s.appKey = applicationKeyFor(s.binary, s.appName, QString());
        s.active = activeByPid.value(pid);
        if (s.appKey.isEmpty())
            continue;
        out.append(s);
    }
    markProcessesContainingUs(out, parents, ourPid);
    return out;
}

#endif // Q_OS_WIN

} // namespace

#endif // HAVE_LIGHTNING_WEBRTC

SystemExclusion systemExclusion()
{
#if defined(HAVE_LIGHTNING_WEBRTC) && (defined(Q_OS_WIN) || defined(Q_OS_LINUX))
    if (!lightning::gst::ensureInitialised())
        return systemExclusionFor(Platform::Other, {});
#if defined(Q_OS_WIN)
    return systemExclusionFor(Platform::Windows, loadedExclusionElements());
#else
    return systemExclusionFor(Platform::Linux, loadedExclusionElements());
#endif
#else
    // macOS has no loopback capture; without the engine nothing captures.
    return systemExclusionFor(Platform::Other, {});
#endif
}

SystemCapture systemCapture(qint64 ourPid)
{
#if defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_WIN)
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
    case Platform::Other:
        break;
    }
    return Unavailability::NoCapture;
}

Unavailability systemUnavailability()
{
#if defined(HAVE_LIGHTNING_WEBRTC) && defined(Q_OS_WIN)
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
    // A fresh connection every pass, so a restarted daemon is simply the
    // daemon this pass reaches (see pipewireDevicePassLocked()).
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
    // The guard is only ever READ on the GUI thread (inside the queued call),
    // where the context is also destroyed; the worker only copies it.
    QPointer<QObject> guard(context);
    std::thread([guard, ourPid, ourNames, checkRunning,
                 done = std::move(done)]() mutable {
        Enumeration result;
        try {
            result = enumerate(ourPid, ourNames, checkRunning);
        } catch (...) {
            // An exception escaping a detached thread terminates.
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
/// The probe body for this platform.
static ProbeOutcome runProbe()
{
#if defined(Q_OS_WIN)
    // GStreamer installs the loopback properties only where the OS has
    // process loopback (Windows build 20348+). Each missing piece is named: a
    // package without the audiomixer plugin and an old Windows look identical
    // otherwise. Both are facts about the install, so the answer is definite.
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
#endif // HAVE_LIGHTNING_WEBRTC

bool perApplicationCaptureAvailable(bool reprobe)
{
    // Bounded, because it runs on the GUI thread: starting the PipeWire
    // provider waits for the daemon's first answer, synchronously and without
    // a timeout. Only a DEFINITE answer is cached for the run. A timeout or a
    // daemon that is not up yet answers false for now and is asked again (at
    // most every five seconds, or at once on `reprobe`), because a one-off
    // slow first probe used to switch share sound off until the restart. A
    // blocked probe cannot be cancelled, so its worker is abandoned, and
    // never joined by a second: it stores its own answer when it ends.
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
    // Initialise on the caller's thread: ensureInitialised() is a call_once,
    // and if an abandoned worker were the first caller and hung inside
    // gst_init_check, every later caller would block on the once_flag
    // forever. A failure is not remembered: nothing was learned about audio.
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
        // An exception escaping a detached thread terminates.
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
    // Diagnostic (docs/voice-calls.md): behave as with a PipeWire plugin older
    // than 1.6, which has no on-disconnect (every package before it), so that
    // path can be exercised on a 1.6 host. The engine then retires a departed
    // application's branch itself.
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

} // namespace lightning::shareaudio
