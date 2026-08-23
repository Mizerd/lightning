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

    /// Attach a QML VideoOutput's sink to one sender's stream.
    ///
    /// `streamId` is the LiveKit stream id (the sending participant's sid,
    /// from the SDP `msid`) — the same key the decrypt probes use, so a tile
    /// and its key ring cannot disagree about whose frames these are.
    ///
    /// Registering the same stream again REPLACES the sink: a tile that is
    /// destroyed and rebuilt (a grid relayout, a participant moving between
    /// stage and strip) must not leave the old sink receiving frames.
    Q_INVOKABLE void attachSink(const QString &streamId, QVideoSink *sink);
    /// Detach whatever sink is attached to `streamId`. Safe to call for a
    /// stream that was never attached.
    Q_INVOKABLE void detachSink(const QString &streamId);
    /// Forget every sink. Called on teardown: a stale sink is a dangling
    /// destination for the next call's frames.
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

public Q_SLOTS:
    /// Deliver one frame. Always invoked on the GUI thread (queued from the
    /// streaming thread), because a QVideoSink belongs to its own thread.
    void deliverFrame(const QString &streamId, const QVideoFrame &frame);

private:
    /// Guards m_sinks. attachSink/detachSink/clear run on the GUI thread;
    /// watching() runs on a GStreamer streaming thread.
    mutable QMutex m_mutex;
    /// QPointer, not raw: a VideoOutput can be destroyed between a frame
    /// being queued on the streaming thread and being delivered here, and
    /// that window is exactly one frame wide on every relayout.
    QHash<QString, QPointer<QVideoSink>> m_sinks;
};
