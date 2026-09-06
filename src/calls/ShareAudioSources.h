#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

// Capturing what the computer is playing WITHOUT capturing ourselves.
//
// THE DEFECT THIS EXISTS FOR (tester report 2026-09-06 §3, confirmed twice by
// the maintainer): sharing a screen with sound sent the whole system output
// back into the call, so remote participants heard themselves. The capture
// took the DEFAULT SINK'S MONITOR — `pulsesrc device=@DEFAULT_MONITOR@` — and
// a sink monitor is the post-mix output. Lightning's own playback of everyone
// else is one of the contributors to that mix, and a monitor cannot subtract
// a contributor: by the time the signal exists it has already been summed.
// The only mitigation that ever existed was routing Lightning's own audio to
// a different output device by hand.
//
// WHAT WAS MEASURED BEFORE THIS WAS WRITTEN (2026-09-06, on a live PipeWire
// 1.6.6 desktop; the recipe is in docs/voice-calls.md so the next round does
// not have to guess):
//
//  * The xdg-desktop-portal ScreenCast interface HAS NO AUDIO. Version 5 on
//    this machine offers CreateSession/SelectSources/Start/OpenPipeWireRemote
//    and nothing else, and upstream's own interface XML (version 6) documents
//    no audio option and no audio stream. That route is closed, not pending.
//  * `pipewiresrc target-object=<object.serial>` captures EXACTLY ONE
//    application's playback: measured -9.03 dBFS from the targeted stream and
//    digital silence (-700 dBFS) from a silent one, with pw-link showing the
//    link landing on that node's own output ports.
//  * TARGETING BY `node.name` DOES NOT WORK and does not say so. The same
//    capture, given the stream's name instead of its serial, ran happily and
//    produced digital silence. Only `object.serial` (or the deprecated
//    `path=<node id>`) actually resolves a stream node. Do not "simplify"
//    this to a name.
//  * `stream.capture.sink=true` is NOT needed when the target is a stream
//    node; it is the property for capturing a SINK's monitor.
//  * A capture stream with `autoconnect=false`, linked by hand afterwards,
//    IS A DEAD END: the element never negotiates a format and the pipeline
//    never leaves PAUSED, before and after the links exist. So "one capture
//    stream, many hand-made links" — which is how OBS does it — cannot be
//    built out of GStreamer's element. One `pipewiresrc` per application,
//    summed by an `audiomixer`, is the shape that works.
//  * The enumeration needs NO new dependency. `GstDeviceMonitor` reports
//    `Stream/Output/Audio` devices through the PipeWire device provider that
//    ships with the same plugin the video share already uses, and
//    `gst_device_get_properties()` carries `object.serial`,
//    `application.process.id`, `application.name` and `node.name` —
//    everything needed to target a stream and to recognise our own.
//
// THE POLICY IS THE FIX AND IT IS PURE ON PURPOSE. Which streams the share
// carries is one decision, `streamIsForeign()` below, taking a property map
// and answering yes or no. It needs no PipeWire, no GStreamer and no call, so
// it is unit-tested directly; everything around it is plumbing.
//
// SCOPE, HONESTLY. This is the Linux/PipeWire path only. Windows captures
// through `wasapi2src loopback=true`, which is the endpoint mix and has the
// same echo for the same reason; nothing here changes that, and the picker
// says so. macOS has no loopback capture at all.
namespace lightning::shareaudio {

/// One application playback stream, as PipeWire describes it.
struct Stream {
    /// `object.serial`. The ONLY identifier `pipewiresrc target-object`
    /// actually resolves for a stream node — see the header note. Digits
    /// only, and validated as such before it is ever put in a parse string.
    QString serial;
    /// `application.name`, for the log line. Presentation only, never a key.
    QString appName;
    /// `node.name`, for the log line when there is no application name.
    QString nodeName;
    /// `application.process.id`, or -1 when the node does not carry one.
    qint64 pid = -1;
};

/// Should the share carry this stream?
///
/// `props` is a PipeWire node's property map as `gst_device_get_properties()`
/// reports it. `ourPid` is this process, `ourClientName` the name this
/// process's own playback appears under.
///
/// False for our own playback — that exclusion IS the echo fix — and false
/// for anything that cannot be targeted or is not an application.
bool streamIsForeign(const QVariantMap &props, qint64 ourPid,
                     const QString &ourClientName);

/// Read one stream out of a property map. `serial` is empty when the map
/// does not describe a targetable stream.
Stream streamFromProperties(const QVariantMap &props);

/// The per-application capture description, ending on the mixer so the
/// caller can append its own encoder chain with `! `.
///
/// Always contains a silence floor. A share started before anything is
/// playing, or one whose every application has since gone away, must keep
/// handing the Opus encoder a timeline — otherwise the track publishes and
/// then dies, which is a worse failure than the echo it replaces.
QString mixedSourceDescription(const QList<Stream> &streams);

/// One application's branch, for adding to a share already running. Ends on
/// the caps the mixer expects; the caller links it to a requested pad.
QString applicationBranchDescription(const Stream &stream, int index);

/// The name the mixer is given in `mixedSourceDescription`.
QString mixerElementName();

/// Can this machine capture per application at all?
///
/// Answered once and cached: it probes GStreamer and opens a PipeWire
/// connection, and the UI asks it from a binding. False means the share falls
/// back to the output monitor, which carries this call's own audio — so the
/// picker can say which of the two the user is about to get instead of
/// leaving them to find out from the far end.
bool perApplicationCaptureAvailable();

/// Live enumeration. Every method is safe to call on a build without the
/// media engine, where `start()` simply answers false.
class SourceMonitor
{
public:
    SourceMonitor() = default;
    ~SourceMonitor();
    SourceMonitor(const SourceMonitor &) = delete;
    SourceMonitor &operator=(const SourceMonitor &) = delete;

    /// True when this machine can capture per application: the PipeWire
    /// device provider is registered, `audiomixer` exists, and `pipewiresrc`
    /// has `on-disconnect`. A property a packaged plugin may not have is
    /// checked, never assumed (§16).
    ///
    /// `on-disconnect=eos` is what makes a departing application retire its
    /// own branch. Stated precisely, because an over-stated mechanism is how
    /// a wrong generalisation gets made: this chain is LIVE (`audiotestsrc
    /// is-live=true` plus `pipewiresrc`), and `GstAggregator` in a live
    /// pipeline does not wait on a silent pad forever — it times out against
    /// its latency deadline and substitutes silence. So the cost of a pad
    /// that stops speaking without EOSing is added latency and a wasted pad,
    /// not a dead track. Refusing per-application capture without the
    /// property is conservatism, not necessity, and saying which it is
    /// matters more than the refusal.
    bool start();
    void stop();
    bool running() const { return m_monitor != nullptr; }

    /// The application streams playing right now, ours excluded.
    QList<Stream> streams(qint64 ourPid, const QString &ourClientName) const;

private:
    void *m_monitor = nullptr; // GstDeviceMonitor *, opaque so this header
                               // stays free of GStreamer for the pure tests.
};

} // namespace lightning::shareaudio
