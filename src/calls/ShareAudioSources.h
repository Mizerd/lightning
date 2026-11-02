#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class QObject;

struct _GObject;

namespace lightning::shareaudio {

/// What a share's sound carries. Persisted as an int: unknown values read as
/// Off (an enum is not a quantity, so a value from a newer build must not be
/// clamped onto the nearest mode).
enum class Mode : int {
    Off = 0,
    System = 1,
    Apps = 2,
};
Mode modeFromInt(int value);

enum class Capture {
    PipeWire,
    WasapiProcess,
    Test,
};

struct Stream {
    QString serial;
    QString appName;
    QString nodeName;
    QString binary;
    QString iconName;
    QString appKey;
    qint64 pid = -1;
    qint64 startTime = 0;
    bool active = true;
    /// Windows: the process is an ANCESTOR of Lightning (explorer.exe, a
    /// launcher, a terminal). include-process-tree on it would capture
    /// Lightning's own playback, the echo, so it is listed but never captured.
    bool containsUs = false;

    QString id() const;
};

QString applicationKeyFor(const QString &appName, const QString &binary,
                          const QString &nodeName);

QString applicationLabel(const Stream &stream);

QString sanitizedIconName(const QString &name);

struct Selection {
    Mode mode = Mode::System;
    QStringList keys;

    bool wants(const Stream &stream) const;
    bool wantsKey(const QString &appKey) const;
    bool capturesAnything() const;
    bool operator==(const Selection &other) const;
    bool operator!=(const Selection &other) const { return !(*this == other); }
};

struct Application {
    QString key;
    QString label;
    QString iconName;
    int streams = 0;
    bool active = false;
    bool containsUs = false;
};

QList<Application> groupByApplication(const QList<Stream> &streams);

bool streamIsForeign(const QVariantMap &props, qint64 ourPid,
                     const QStringList &ourNames);

Stream streamFromProperties(const QVariantMap &props);

struct BranchOptions {
    Capture capture = Capture::PipeWire;
    bool retireOnDisconnect = true;
    bool followsProcess = false;
};

QString applicationBranchDescription(const Stream &stream, int index,
                                     const BranchOptions &options = {},
                                     bool muted = false);

QString branchVolumeName(int index);
QString branchSourceName(int index);
QString branchBinName(int index);
int branchIndexFromElementName(const QString &name);

QString mixedSourceDescription(const QList<Stream> &streams,
                               const BranchOptions &options = {});

QString mixerElementName();

QString levelElementName();

QString encodedTrackDescription(const QString &sourceDescription,
                                quint32 ssrc, bool withLevel = false,
                                bool muted = false);

QString shareValveElementName();

QList<qint64> dropDescendants(const QList<qint64> &pids,
                              const QHash<qint64, qint64> &parentOf);

QSet<qint64> ancestorsOf(qint64 pid, const QHash<qint64, qint64> &parentOf);

void markProcessesContainingUs(QList<Stream> &streams,
                               const QHash<qint64, qint64> &parentOf,
                               qint64 ourPid);

struct BranchRecord {
    int index = -1;
    QString appKey;
    qint64 pid = -1;
    qint64 startTime = 0;
    bool muted = false;
    bool retired = false;
    bool failed = false;
    bool starting = false;
};

struct ScanInput {
    QList<Stream> live;
    QHash<QString, BranchRecord> records;
    Selection selection;
    BranchOptions options;
    QSet<QString> running;
    QHash<qint64, qint64> parents;
    int liveBranches = 0;
    int cap = 24;
};

struct ScanPlan {
    QList<Stream> add;
    QStringList retire;
    QList<QPair<QString, bool>> mute;
    QStringList carried;
    bool limitReached = false;
};

ScanPlan planScan(const ScanInput &in);

struct Enumeration {
    QList<Stream> streams;
    QHash<qint64, qint64> parents;
    QSet<QString> running;
    qint64 elapsedMs = 0;
};

Enumeration enumerate(qint64 ourPid, const QStringList &ourNames,
                      const QList<Stream> &checkRunning = {});

QHash<qint64, qint64> processParents();

void enumerateAsync(QObject *context, qint64 ourPid,
                    const QStringList &ourNames,
                    const QList<Stream> &checkRunning,
                    std::function<void(const Enumeration &)> done);

// Entire-system capture must exclude our call playback. Missing plugins or
// process-loopback support disable share audio, rather than capture a mix
// containing the other participants' voices.

struct ExclusionElements {
    bool audiomixer = false;
    bool pipewireTargetObject = false;
    bool pipewireDeviceProvider = false;
    bool wasapiProcessLoopback = false;
    bool wasapiSource = false;
    bool screenCaptureKit = false;
};

enum class Platform { Linux, Windows, MacOS, Other };

struct SystemExclusion {
    bool possible = false;
    QString how;
    QStringList missing;
};

SystemExclusion systemExclusionFor(Platform platform,
                                   const ExclusionElements &elements);

SystemExclusion systemExclusion();

QString systemExclusionStatusLine(const SystemExclusion &verdict);

struct SystemCapture {
    enum class Kind { Unavailable, PerApplication, ProcessLoopback };
    Kind kind = Kind::Unavailable;
    QString description;
    bool excludesUs = false;
};

SystemCapture systemCaptureFor(Platform platform, bool perApplicationAvailable,
                               bool processLoopbackAvailable, qint64 ourPid);
SystemCapture systemCapture(qint64 ourPid);

enum class Unavailability {
    None,
    NoCapture,
    MissingPlugin,
    NoPipeWire,
    OldWindows
};

Unavailability unavailabilityFor(Platform platform,
                                 const ExclusionElements &elements,
                                 bool daemonReached);

Unavailability systemUnavailability();

enum class ProbeOutcome { Unavailable, Available, Unknown };

bool perApplicationCaptureAvailable(bool reprobe = false);

void setPerApplicationProbeForTest(std::function<ProbeOutcome()> probe,
                                   int timeoutMs = 2500);
void resetPerApplicationProbeForTest();

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

int givePrivatePipeWireConnection(::_GObject *object);

QList<QVariantMap> pipewireDevices(bool *reached);

/// Has the daemon closed this connection (the far end hung up)? Never
/// blocks. A capture branch whose connection closed is dead: pipewiresrc
/// neither errors nor sends EOS when the daemon goes away (its stream merely
/// becomes unconnected), so without this a branch whose serial a restarted
/// daemon handed to the same application's new stream carried silence for
/// the rest of the share. Measured live.
bool connectionClosed(int fd);

BranchOptions branchOptions();

class SourceMonitor
{
public:
    bool start();
    void stop() { m_running = false; }
    bool running() const { return m_running; }
    QList<Stream> streams(qint64 ourPid, const QStringList &ourNames) const;

private:
    bool m_running = false;
};

}
