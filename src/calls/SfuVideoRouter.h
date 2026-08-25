// Remote video frames from the SFU pipeline to a QML VideoOutput.
//
// Until this existed, received video went to a `fakesink`: the pipeline
// decoded every frame correctly and then threw it away, so a video call
// showed nothing. This is the missing half.
//
// Why not a GStreamer QML sink: the pinned dev shell has neither
// `qml6glsink` nor `qmlglsink` (checked with gst-inspect), and a packaged
// build cannot depend on a plugin that may not be there. `appsink` is in
// gst-plugins-base and is always present, so frames are pulled out and
// handed to Qt's own QVideoSink — the same object a QML `VideoOutput`
// exposes, so the existing declarative surface renders them with no new
// plugin dependency.
//
// Threading: frames arrive on a GStreamer streaming thread and are DELIVERED
// on the GUI thread through a queued connection. A QVideoSink must only be
// touched from the thread that owns it.
//
// Privacy: a video frame is call content. Nothing here logs frame bytes,
// dimensions per frame, or timing; failures are counted, not described.
//
// OWNERSHIP (2026-08-27). One sink per key, and a key is CLAIMED by whoever
// attached last. A release names the SINK and never a key, so it can only
// ever give up what that sink still owns — a surface that has been superseded
// releases nothing.
//
// That rule is not decoration; without it the surface above this class could
// not work at all. Qt destroys a deactivated Loader's content and a
// regenerated Repeater's delegates with `deleteLater()`, while it creates the
// replacements SYNCHRONOUSLY — so the order on every grid↔spotlight swap and
// on every participant reorder is: NEW tile attaches, THEN old tile detaches.
// With a detach that removed by key alone, the dying tile unhooked the live
// one every single time, and because a tile only attaches on creation and on a
// routing-key change, nothing ever put it back: the video was gone for the
// rest of the call.
//
// That is exactly the maintainer's "camera no longer works" and "when i full
// screen it it stop shwoing video". Before 2026-08-26 it was masked by
// accident — the stage bound a JS array rebuilt on every update, so every tile
// was destroyed and re-created continuously and re-attached itself several
// times a second. Removing that churn (which is what made an amplitude ring
// possible) exposed a defect that had been here since the router was written.
#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVideoFrame>

class QVideoSink;

class SfuVideoRouter : public QObject
{
    Q_OBJECT

public:
    explicit SfuVideoRouter(QObject *parent = nullptr);

    /// Attach a QML VideoOutput's sink to one sender's stream. This CLAIMS
    /// the key.
    ///
    /// `streamId` is the LiveKit stream id (the sending participant's sid,
    /// from the SDP `msid`) — the same key the decrypt probes use, so a tile
    /// and its key ring cannot disagree about whose frames these are.
    ///
    /// Registering the same stream again REPLACES the sink: a tile that is
    /// destroyed and rebuilt (a grid relayout, a participant moving between
    /// stage and strip) must not leave the old sink receiving frames. The
    /// LAST attach wins, which is what makes the handover in the comment
    /// above work: the surface being BUILT claims the key, and the surface
    /// being torn down can no longer take it away.
    ///
    /// A NULL sink is a no-op, deliberately. It used to remove the key, and
    /// "I have no sink" is not the same statement as "nobody may have this
    /// key" — a VideoOutput whose sink has not materialised yet would have
    /// evicted a working surface.
    Q_INVOKABLE void attachSink(const QString &streamId, QVideoSink *sink);

    /// Release `sink` from every key it currently OWNS, and nothing else.
    ///
    /// THE ONLY per-surface removal, and it deliberately names no key. Two
    /// separate things went wrong when a key named it:
    ///
    ///  * A release from a SUPERSEDED owner tore down the live one. That is
    ///    the whole regression — see the ownership note at the top.
    ///  * The key a tile attached under is DERIVED (from a track sid that
    ///    arrives late and can change), so a tile recomputing it at
    ///    destruction time could name the wrong one honestly.
    ///
    /// Naming the sink cannot be wrong in either way: a surface gives up
    /// exactly what it holds, and a surface that holds nothing gives up
    /// nothing. Entries whose QPointer has already gone null are swept at the
    /// same time — they route nothing, and holding one keeps a dead key
    /// occupied against the surface that wants it next.
    void releaseSink(QVideoSink *sink);

    /// Forget every sink. Called on teardown: a stale sink is a dangling
    /// destination for the next call's frames.
    ///
    /// This is the ONLY unconditional removal left, and it takes no key.
    /// There is deliberately no `detachSink(key)` any more — not renamed, not
    /// made private, GONE — because that signature is the defect: it was
    /// reachable from QML, it removed whatever happened to be there, and a
    /// dying tile therefore unhooked the tile that had just replaced it.
    /// Leaving a one-argument removal in the class is an invitation to reach
    /// for it the next time a tile lifecycle needs tidying.
    void clear();

    /// Whether anything is listening for this stream. The engine checks this
    /// before COPYING a frame, so an unwatched participant's video costs a
    /// pointer lookup rather than a full-frame memcpy per frame.
    ///
    /// Called from a GStreamer STREAMING THREAD, which is why everything
    /// here is under a mutex: QHash is not safe to read while another thread
    /// inserts, and a rehash during that read is a crash, not merely a wrong
    /// answer.
    bool watching(const QString &streamId) const;

    /// Is this EXACT sink the registered owner of `streamId`?
    ///
    /// The property a test has to assert. `watching()` alone cannot
    /// distinguish "the new surface owns it" from "the dying surface still
    /// does", and those are the two states the ownership rule exists to keep
    /// apart.
    bool watchedBy(const QString &streamId, const QVideoSink *sink) const;

public Q_SLOTS:
    /// Deliver one frame. Always invoked on the GUI thread (queued from the
    /// streaming thread), because a QVideoSink belongs to its own thread.
    void deliverFrame(const QString &streamId, const QVideoFrame &frame);

private:
    /// Guards m_sinks. attach/release/clear run on the GUI thread;
    /// watching() runs on a GStreamer streaming thread.
    mutable QMutex m_mutex;
    /// QPointer, not raw: a VideoOutput can be destroyed between a frame
    /// being queued on the streaming thread and being delivered here, and
    /// that window is exactly one frame wide on every relayout.
    QHash<QString, QPointer<QVideoSink>> m_sinks;
};
