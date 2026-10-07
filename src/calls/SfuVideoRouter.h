// Remote video frames from the SFU pipeline to a QML VideoOutput.
//
// Frames come out through `appsink` (always present in gst-plugins-base) and
// go to Qt's own QVideoSink, which QML's VideoOutput exposes, so no GStreamer
// QML sink plugin is required.
//
// Threading: frames arrive on a streaming thread and are delivered on the GUI
// thread, which owns the QVideoSink.
//
// Privacy: frames are call content; nothing here logs their bytes, sizes or
// timing.
//
// Ownership: one sink per key, claimed by whoever attached last. A release
// names the sink, never a key, so it only gives up what that sink still owns.
// Qt creates replacement tiles synchronously but destroys old ones with
// deleteLater(), so on every relayout the new tile attaches before the old
// one detaches; a key-based detach would unhook the live tile.
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

    /// Attach a VideoOutput's sink to a stream, claiming the key. `streamId`
    /// is the routing key (a track sid, or the sender's LiveKit sid) that the
    /// engine delivers frames under. The last attach wins, so a rebuilt tile
    /// takes over from the old one. A null sink is a no-op, so a VideoOutput
    /// without a sink yet cannot evict a working surface.
    Q_INVOKABLE void attachSink(const QString &streamId, QVideoSink *sink);

    /// Release `sink` from every key it currently owns, and nothing else. Keys
    /// are derived and can change, so only the sink identifies what a surface
    /// holds. Entries whose QPointer went null are swept at the same time.
    void releaseSink(QVideoSink *sink);

    /// Forget every sink, on teardown. There is intentionally no per-key
    /// removal: it would let a dying tile unhook its replacement.
    void clear();

    /// Whether anything watches this stream. Checked before copying a frame,
    /// so an unwatched stream costs a lookup, not a memcpy. Called on a
    /// streaming thread, hence the mutex.
    bool watching(const QString &streamId) const;

    /// Is this exact sink the owner of `streamId`? Lets tests tell the new
    /// owner from the dying one, which watching() cannot.
    bool watchedBy(const QString &streamId, const QVideoSink *sink) const;

    /// Leave the newest frame for `streamId` to be delivered, from a
    /// streaming thread. Replaces a frame still waiting for that key, so at
    /// most ONE frame per key is ever held, however long the GUI thread is
    /// busy. Returns true when no delivery is scheduled for the key yet: the
    /// caller then schedules exactly one deliverPending() on the GUI thread.
    ///
    /// Why: every frame used to be queued to the GUI thread as its own event.
    /// A GUI thread blocked for 30 s (a pipewiresrc start, measured live in
    /// the Flatpak) collected 900 frames of a 1080p share's self-view, 8 MB
    /// each: RSS 1.1 -> 7.5 GB, and the host went down.
    bool offerFrame(const QString &streamId, const QVideoFrame &frame);

    /// Frames waiting across all keys (tests).
    int pendingFramesForTest() const;

public Q_SLOTS:
    /// Deliver one frame, on the GUI thread (queued from the streaming
    /// thread).
    void deliverFrame(const QString &streamId, const QVideoFrame &frame);

    /// Deliver the frame offerFrame() left for `streamId`, on the GUI thread.
    void deliverPending(const QString &streamId);

private:
    /// Guards m_sinks and m_pending: attach/release/clear run on the GUI
    /// thread, watching() and offerFrame() on a streaming thread.
    mutable QMutex m_mutex;
    /// QPointer: a VideoOutput can be destroyed between a frame being queued
    /// and delivered.
    QHash<QString, QPointer<QVideoSink>> m_sinks;
    /// The newest undelivered frame per key; present means a delivery is
    /// scheduled.
    QHash<QString, QVideoFrame> m_pending;
};
