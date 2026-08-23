#include "calls/SfuVideoRouter.h"

#include <QMutexLocker>
#include <QVideoSink>

SfuVideoRouter::SfuVideoRouter(QObject *parent) : QObject(parent) {}

void SfuVideoRouter::attachSink(const QString &streamId, QVideoSink *sink)
{
    if (streamId.isEmpty())
        return;
    if (!sink) {
        detachSink(streamId);
        return;
    }
    QMutexLocker lock(&m_mutex);
    m_sinks.insert(streamId, sink);
}

void SfuVideoRouter::detachSink(const QString &streamId)
{
    QMutexLocker lock(&m_mutex);
    m_sinks.remove(streamId);
}

void SfuVideoRouter::clear()
{
    QMutexLocker lock(&m_mutex);
    m_sinks.clear();
}

bool SfuVideoRouter::watching(const QString &streamId) const
{
    QMutexLocker lock(&m_mutex);
    const auto it = m_sinks.constFind(streamId);
    // A QPointer that has gone null is not a watcher. Reporting it as one
    // would make the engine copy a frame per frame for a destroyed tile.
    return it != m_sinks.cend() && !it->isNull();
}

void SfuVideoRouter::deliverFrame(const QString &streamId,
                                  const QVideoFrame &frame)
{
    QVideoSink *sink = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_sinks.constFind(streamId);
        if (it == m_sinks.cend())
            return;
        sink = it->data();
        if (!sink) {
            // The tile went away while this frame was in flight. Drop the
            // stale entry so the next frame does not pay the lookup again.
            m_sinks.remove(streamId);
            return;
        }
    }
    // Outside the lock: setVideoFrame reaches into the render path, and
    // holding a mutex the streaming thread also wants across it would stall
    // the pipeline behind the compositor.
    sink->setVideoFrame(frame);
}
