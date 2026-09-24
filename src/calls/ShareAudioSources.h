#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// Capturing what the computer is playing without capturing ourselves.
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
//  * `stream.capture.sink=true` is only for sink monitors, not stream nodes.
//  * A capture with `autoconnect=false`, linked by hand, never negotiates, so
//    OBS-style "one stream, many links" is impossible with this element. One
//    `pipewiresrc` per application, summed by an `audiomixer`, works.
//  * Enumeration needs no new dependency: GstDeviceMonitor reports
//    `Stream/Output/Audio` devices via the PipeWire provider, with
//    `object.serial`, `application.process.id`, `application.name` and
//    `node.name`.
//
// The selection policy is the pure function streamIsForeign(), unit-tested
// without PipeWire or GStreamer.
//
// Linux/PipeWire only. Windows excludes our process tree in `wasapi2src`
// (see shareAudioSourceDescription in SfuMediaEngine.cpp). Linux without
// PipeWire falls back to the echoing sink monitor, and the picker says so;
// macOS has no loopback capture.
namespace lightning::shareaudio {

/// One application playback stream, as PipeWire describes it.
struct Stream {
    /// `object.serial`, the only identifier `pipewiresrc target-object`
    /// resolves for a stream node. Validated as digits before use in a parse
    /// string.
    QString serial;
    /// `application.name`, for logging only.
    QString appName;
    /// `node.name`, for logging when there is no application name.
    QString nodeName;
    /// `application.process.id`, or -1 when the node does not carry one.
    qint64 pid = -1;
};

/// Should the share carry this stream? `props` is a node's property map from
/// gst_device_get_properties(). False for our own playback (the echo fix) and
/// for anything that is not a targetable application stream.
///
/// `ourNames` lists every name this process may appear under: PipeWire
/// records our playback under the binary name ("lightning-matrix"), not
/// QCoreApplication::applicationName().
bool streamIsForeign(const QVariantMap &props, qint64 ourPid,
                     const QStringList &ourNames);

/// Reads one stream from a property map; `serial` is empty when the map does
/// not describe a targetable stream.
Stream streamFromProperties(const QVariantMap &props);

/// The per-application capture description, ending on the mixer so the caller
/// can append its encoder chain with `! `. Always includes a silence floor, so
/// the encoder keeps a timeline when nothing is playing.
QString mixedSourceDescription(const QList<Stream> &streams);

/// One application's branch, for adding to a running share. Ends on the caps
/// the mixer expects; the caller links it to a requested pad.
QString applicationBranchDescription(const Stream &stream, int index);

/// The name the mixer is given in `mixedSourceDescription`.
QString mixerElementName();

/// The whole share-audio bin: a source description plus the encode and
/// payload chain linked to webrtcbin. Built here so tests parse exactly what
/// production builds. mixedSourceDescription() ends in a pad reference
/// (`shareaudiomix.`), which accepts no assignments, so every source
/// description names its own capture element (`name=sharesrc`) and nothing is
/// appended to it here.
QString encodedTrackDescription(const QString &sourceDescription,
                                quint32 ssrc);

/// Can this machine capture per application? Answered once and cached (it
/// probes GStreamer and PipeWire, and the UI asks from a binding). False means
/// the share falls back to the output monitor, which includes this call's
/// audio; the picker tells the user which they get.
bool perApplicationCaptureAvailable();

/// Live enumeration. Safe on builds without the media engine, where start()
/// returns false.
class SourceMonitor
{
public:
    SourceMonitor() = default;
    ~SourceMonitor();
    SourceMonitor(const SourceMonitor &) = delete;
    SourceMonitor &operator=(const SourceMonitor &) = delete;

    /// True when per-application capture is possible: the PipeWire device
    /// provider is registered, `audiomixer` exists, and `pipewiresrc` has
    /// `on-disconnect` (checked, since packaged plugin versions vary).
    ///
    /// `on-disconnect=eos` lets a departing application retire its branch.
    /// Without it, the live aggregator would substitute silence after its
    /// latency deadline rather than stall, so requiring it is caution rather
    /// than necessity.
    bool start();
    void stop();
    bool running() const { return m_monitor != nullptr; }

    /// The application streams playing right now, ours excluded.
    QList<Stream> streams(qint64 ourPid, const QStringList &ourNames) const;

private:
    void *m_monitor = nullptr; // GstDeviceMonitor *, opaque so this header
                               // stays free of GStreamer for the pure tests.
};

} // namespace lightning::shareaudio
