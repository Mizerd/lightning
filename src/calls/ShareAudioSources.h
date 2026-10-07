#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class QObject;

// Capturing what the computer is playing without capturing ourselves, and
// choosing WHICH applications a share carries.
//
// A sink monitor (`pulsesrc device=@DEFAULT_MONITOR@`) is the post-mix
// output, which includes Lightning's own playback of the other participants,
// so sharing a screen with sound echoed the call back to everyone. This
// captures each foreign application's stream instead. Verified on PipeWire
// 1.6 (recipe in docs/voice-calls.md):
//
//  * The xdg ScreenCast portal carries no audio at all.
//  * `pipewiresrc target-object=<object.serial>` captures exactly one
//    application's playback. Targeting by `node.name` silently yields digital
//    silence; only `object.serial` resolves a stream node.
//  * A capture with `autoconnect=false`, linked by hand, never negotiates, so
//    OBS-style "one stream, many links" is impossible with this element. One
//    `pipewiresrc` per application, summed by an `audiomixer`, works.
//  * Enumeration needs no new dependency: the PipeWire GstDeviceProvider
//    reports `Stream/Output/Audio` nodes with `object.serial`,
//    `application.process.id`, `application.name` and `node.name`.
//
// On Windows (10 2004+, where wasapi2src has `loopback-target-pid`) the same
// shape is built from one `wasapi2src loopback-mode=include-process-tree` per
// chosen application's audio session; "the whole system except us" stays the
// single exclude-process-tree capture in SfuMediaEngine.cpp.
//
// Which streams a share carries, and what a running share does as streams come
// and go, are pure functions here (streamIsForeign, Selection::wants,
// planScan), unit-tested without PipeWire, WASAPI or GStreamer. Linux without
// a reachable PipeWire falls back to the echoing sink monitor, and the UI says
// so; macOS has no loopback capture.

struct _GObject;   // GLib's GObject, without pulling GLib into this header

namespace lightning::shareaudio {

/// What a share's sound carries. Persisted as an int: unknown values read as
/// Off (an enum is not a quantity, so a value from a newer build must not be
/// clamped onto the nearest mode).
enum class Mode : int {
    /// No share-audio track at all.
    Off = 0,
    /// Everything this computer plays except Lightning itself.
    System = 1,
    /// Only the applications the user chose.
    Apps = 2,
};
Mode modeFromInt(int value);

/// How an application's audio is captured.
enum class Capture {
    /// `pipewiresrc target-object=<object.serial>`.
    PipeWire,
    /// `wasapi2src loopback-mode=include-process-tree loopback-target-pid=<pid>`.
    WasapiProcess,
    /// `audiotestsrc`, for engine tests only (SfuMediaEngine's test seam).
    Test,
};

/// One application playback stream (PipeWire) or audio session (Windows).
struct Stream {
    /// PipeWire: `object.serial`, the only identifier `pipewiresrc
    /// target-object` resolves for a stream node. Windows: the session's
    /// process id. Validated as digits before use in a parse string.
    QString serial;
    /// `application.name`, or on Windows the executable's description.
    QString appName;
    /// `node.name`, for logging when there is no application name.
    QString nodeName;
    /// `application.process.binary`, or on Windows the executable's file name.
    QString binary;
    /// `application.icon-name` (an XDG icon name, [A-Za-z0-9._-] only).
    QString iconName;
    /// The selection key every stream of one application shares; see
    /// applicationKeyFor(). Never empty for a targetable stream.
    QString appKey;
    /// `application.process.id`, or -1 when the node does not carry one.
    qint64 pid = -1;
    /// Windows: the process's creation time (FILETIME ticks), so a reused pid
    /// is a different stream. 0 on PipeWire, whose serials are never reused.
    qint64 startTime = 0;
    /// Windows: whether the session is playing right now (an inactive session
    /// is still capturable). PipeWire nodes exist only while a stream is open.
    bool active = true;
    /// Windows: the process is an ANCESTOR of Lightning (explorer.exe, a
    /// launcher, a terminal). include-process-tree on it would capture
    /// Lightning's own playback, the echo, so it is listed but never captured.
    bool containsUs = false;

    /// The identity a running share keys its branch by: the serial, plus the
    /// creation time where pids can be reused.
    QString id() const;
};

/// The key that groups an application's streams, so choosing "Firefox" also
/// takes the stream Firefox opens a minute later. Lower-cased; the
/// application's own name first (what pavucontrol shows), then its binary,
/// then the node name. Empty when all three are empty.
QString applicationKeyFor(const QString &appName, const QString &binary,
                          const QString &nodeName);

/// What a person would call the application.
QString applicationLabel(const Stream &stream);

/// An XDG icon name as the audio server reported it, or empty when it is not
/// a plain name ([A-Za-z0-9._-], at most 64): it reaches QIcon::fromTheme.
QString sanitizedIconName(const QString &name);

/// Which streams a share carries.
struct Selection {
    Mode mode = Mode::System;
    /// Application keys, for Mode::Apps.
    QStringList keys;

    /// Never true for a stream that contains Lightning (`containsUs`).
    bool wants(const Stream &stream) const;
    bool wantsKey(const QString &appKey) const;
    /// Whether anything at all would be captured: Off, and Apps with nothing
    /// chosen, publish no track.
    bool capturesAnything() const;
    bool operator==(const Selection &other) const;
    bool operator!=(const Selection &other) const { return !(*this == other); }
};

/// One application, as the picker lists it.
struct Application {
    QString key;
    QString label;
    QString iconName;
    /// Streams (sessions) it has open now.
    int streams = 0;
    /// Whether any of them is playing (Windows reports idle sessions).
    bool active = false;
    /// Lightning runs inside it; it cannot be captured on its own.
    bool containsUs = false;
};

/// Groups streams by application, in first-seen order.
QList<Application> groupByApplication(const QList<Stream> &streams);

/// Should the share be able to carry this stream? `props` is a node's
/// property map from gst_device_get_properties(). False for our own playback
/// (the echo fix) and for anything that is not a targetable application
/// stream.
///
/// `ourNames` lists every name this process may appear under: PipeWire
/// records our playback under the binary name ("lightning-matrix"), not
/// QCoreApplication::applicationName().
bool streamIsForeign(const QVariantMap &props, qint64 ourPid,
                     const QStringList &ourNames);

/// Reads one stream from a property map; `serial` is empty when the map does
/// not describe a targetable stream.
Stream streamFromProperties(const QVariantMap &props);

/// How a branch is built, decided by what the loaded plugins can do.
struct BranchOptions {
    Capture capture = Capture::PipeWire;
    /// `pipewiresrc on-disconnect=eos`, which exists only from PipeWire 1.6.
    /// Without it a departed application's branch is retired by the engine.
    bool retireOnDisconnect = true;
    /// Whether a branch lives as long as its PROCESS rather than its stream.
    /// Windows process loopback follows the process: a session that expires
    /// (a paused browser) or is dropped from the listing is not a reason to
    /// stop capturing; only the process exiting is. PipeWire captures one
    /// stream node, which is gone when its stream is.
    bool followsProcess = false;
};

/// One application's branch, for adding to a running share. Ends on a
/// `volume` named branchVolumeName(index), which is how a branch is silenced
/// when its application is deselected mid-share. The caller links its src pad
/// to a requested mixer pad.
QString applicationBranchDescription(const Stream &stream, int index,
                                     const BranchOptions &options = {},
                                     bool muted = false);

/// The `volume` element ending branch `index`, and the source starting it.
QString branchVolumeName(int index);
QString branchSourceName(int index);
/// The bin a running share wraps branch `index` in; -1 when `name` is not a
/// branch source or bin.
QString branchBinName(int index);
int branchIndexFromElementName(const QString &name);

/// The per-application capture description, ending on the mixer so the caller
/// can append its encoder chain with `! `. Always includes a silence floor, so
/// the encoder keeps a timeline when nothing is playing (or nothing chosen is).
QString mixedSourceDescription(const QList<Stream> &streams,
                               const BranchOptions &options = {});

/// The name the mixer is given in `mixedSourceDescription`.
QString mixerElementName();

/// The `level` element in the encoded track, when the build has one.
QString levelElementName();

/// The whole share-audio bin: a source description plus the encode and
/// payload chain linked to webrtcbin. Built here so tests parse exactly what
/// production builds. mixedSourceDescription() ends in a pad reference
/// (`shareaudiomix.`), which accepts no assignments, so every source
/// description names its own capture element (`name=sharesrc`) and nothing is
/// appended to it here. `withLevel` adds a `level` meter (posting once a
/// second) so a share carrying silence can be told from one carrying sound.
QString encodedTrackDescription(const QString &sourceDescription,
                                quint32 ssrc, bool withLevel = false);

/// Windows: drops every pid whose ancestor (per `parentOf`) is also listed,
/// because include-process-tree on the ancestor already captures it and a
/// second branch would play it twice. Applied only among CHOSEN processes.
QList<qint64> dropDescendants(const QList<qint64> &pids,
                              const QHash<qint64, qint64> &parentOf);

/// Every ancestor of `pid` per `parentOf` (bounded; cycles terminate).
QSet<qint64> ancestorsOf(qint64 pid, const QHash<qint64, qint64> &parentOf);

/// Marks the streams whose process is an ancestor of `ourPid`.
void markProcessesContainingUs(QList<Stream> &streams,
                               const QHash<qint64, qint64> &parentOf,
                               qint64 ourPid);

// ── A running share, as a pure plan ──

/// What a running share knows about one branch it built (or tried to).
struct BranchRecord {
    int index = -1;          // element names; -1 when never built
    QString appKey;
    qint64 pid = -1;
    qint64 startTime = 0;    // Windows: process creation time (pid reuse)
    bool muted = false;
    bool retired = false;    // sent EOS; taken out once finished, then forgotten
    bool failed = false;     // would not build, or errored: never retried
};

struct ScanInput {
    QList<Stream> live;                      // this scan's streams
    QHash<QString, BranchRecord> records;    // by Stream::id()
    Selection selection;
    BranchOptions options;
    /// followsProcess: ids of records whose process is still running.
    QSet<QString> running;
    /// followsProcess: the process tree.
    QHash<qint64, qint64> parents;
    /// Branches built and not yet taken out.
    int liveBranches = 0;
    int cap = 24;
};

struct ScanPlan {
    QList<Stream> add;                       // build, in this order
    QStringList retire;                      // ids to send EOS
    QList<QPair<QString, bool>> mute;        // id -> muted, changes only
    QStringList carried;                     // app keys audible afterwards
    bool limitReached = false;
};

/// What one scan of a running per-application share does. Records keep a
/// branch while its stream is live (PipeWire) or its process runs (Windows,
/// where a vanished SESSION is not a vanished capture). A wanted stream with
/// no record gets a branch, up to `cap`; a failed record is never retried; a
/// branch is muted, never unlinked, when its application is deselected or a
/// chosen ancestor process already captures it.
ScanPlan planScan(const ScanInput &in);

// ── Enumeration ──

/// One enumeration pass.
struct Enumeration {
    QList<Stream> streams;
    /// Windows: the process tree, for planScan.
    QHash<qint64, qint64> parents;
    /// Windows: ids among `checkRunning` whose process is still running.
    QSet<QString> running;
    /// How long the pass took, for the log (Windows cost is not free).
    qint64 elapsedMs = 0;
};

/// Enumerates the application streams that exist now, ours excluded. Safe on
/// any thread (COM is initialised for the call on Windows). Empty unless
/// perApplicationCaptureAvailable().
Enumeration enumerate(qint64 ourPid, const QStringList &ourNames,
                      const QList<Stream> &checkRunning = {});

/// Windows: every running process's parent (one Toolhelp snapshot); empty
/// elsewhere.
QHash<qint64, qint64> processParents();

/// enumerate() on a worker thread; `done` runs on the GUI thread, and not at
/// all if `context` was destroyed meanwhile. A pass that hangs costs a worker,
/// never the GUI.
void enumerateAsync(QObject *context, qint64 ourPid,
                    const QStringList &ourNames,
                    const QList<Stream> &checkRunning,
                    std::function<void(const Enumeration &)> done);

/// Can this machine capture per application? Answered once, on a worker
/// bounded at 2.5 s, and cached. The GUI thread never connects to PipeWire:
/// the probe does it on its worker, and every enumeration on its own worker
/// with a FRESH, PRIVATE connection (privatePipeWireConnection()). If the
/// probe times out, nothing touches the provider again (a start abandoned
/// mid-way holds its start lock). False means "the whole system" falls back
/// to the output monitor, which includes this call's audio, and applications
/// cannot be chosen; the UI says so.
bool perApplicationCaptureAvailable();

/// A new connection of our own to the PipeWire daemon (a connected socket),
/// or -1 when the socket cannot be found or reached. Linux only (-1
/// elsewhere).
///
/// Why: the GStreamer PipeWire plugin shares ONE connection among every
/// element and provider in the process that has no `fd` (it keys its cores by
/// fd, and -1 is one key). After the daemon restarts, that connection is dead
/// for as long as anything still holds it — a capture branch, a microphone —
/// and a device provider started on it waits for an answer that never comes,
/// with no timeout (its core error fired before it was listening). Measured:
/// `systemctl --user restart pipewire` under a running per-application share
/// hung every later enumeration. With a private connection per pass and per
/// branch, a restart costs only the connections it killed.
///
/// The caller keeps the fd open while the core made from it lives (the plugin
/// keys its cores by the fd NUMBER, so a number reused too early would join a
/// dead core), then closes it: a pipewiresrc's when the element is finalized
/// (givePrivatePipeWireConnection()), the device provider's right after each
/// pass stops it (pipewireDevices(); the provider is a singleton that is
/// never finalized).
int privatePipeWireConnection();

/// Gives `object` (a pipewiresrc: NOT the device provider, which is a
/// never-finalized singleton) its own connection when it has an `fd`
/// property and one can be opened, and closes that fd when the object is
/// finalized. Returns the fd, or -1 (then the object keeps the plugin's
/// shared connection). Call before the object connects (NULL state).
int givePrivatePipeWireConnection(::_GObject *object);

/// One serialized pass over the PipeWire device provider on a private
/// connection: every node's properties, and whether any was seen. The
/// connection is closed before it returns. Blocks while the daemon answers
/// (no timeout of its own): workers only. Not gated on the probe (enumerate()
/// is). Empty off Linux.
QList<QVariantMap> pipewireDevices(bool *reached);

/// Has the daemon closed this connection (the far end hung up)? Never
/// blocks. A capture branch whose connection closed is dead: pipewiresrc
/// neither errors nor sends EOS when the daemon goes away (its stream merely
/// becomes unconnected), so without this a branch whose serial a restarted
/// daemon handed to the same application's new stream carried silence for
/// the rest of the share. Measured live.
bool connectionClosed(int fd);

/// The branch options this machine's plugins support. Meaningful only when
/// perApplicationCaptureAvailable().
BranchOptions branchOptions();

/// Thin wrapper kept for callers that want an object: start() is
/// perApplicationCaptureAvailable(), streams() one enumerate() pass.
class SourceMonitor
{
public:
    /// Linux: the PipeWire device provider is registered AND reaches a
    /// PipeWire daemon (its start() reports success even when it cannot
    /// connect, so reaching the daemon is checked separately), `audiomixer`
    /// exists, and `pipewiresrc` has `target-object`. Windows: wasapi2src has
    /// `loopback-target-pid` and `audiomixer` exists.
    bool start();
    void stop() { m_running = false; }
    bool running() const { return m_running; }
    QList<Stream> streams(qint64 ourPid, const QStringList &ourNames) const;

private:
    bool m_running = false;
};

} // namespace lightning::shareaudio
