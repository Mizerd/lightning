#include "calls/SfuVideoRouter.h"

#include <QMutexLocker>
#include <QVideoSink>

SfuVideoRouter::SfuVideoRouter(QObject *parent) : QObject(parent) {}

void SfuVideoRouter::attachSink(const QString &streamId, QVideoSink *sink)
{
    if (streamId.isEmpty())
        return;
    if (!sink) {
        // A no-op, NOT a removal. See the header: "I have no sink yet" and
        // "nobody may own this key" are different statements, and treating
        // the first as the second let a half-built surface evict a working
        // one.
        return;
    }
    QMutexLocker lock(&m_mutex);
    m_sinks.insert(streamId, sink);
}

void SfuVideoRouter::releaseSink(QVideoSink *sink)
{
    if (!sink)
        return;
    QMutexLocker lock(&m_mutex);
    for (auto it = m_sinks.begin(); it != m_sinks.end();) {
        // Dead entries go with it. They belong to a surface that was
        // destroyed without ever reaching its release, and nothing else
        // sweeps them until a frame happens to arrive for that key.
        if (it->isNull() || it->data() == sink)
            it = m_sinks.erase(it);
        else
            ++it;
    }
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

bool SfuVideoRouter::watchedBy(const QString &streamId,
                               const QVideoSink *sink) const
{
    if (!sink)
        return false;
    QMutexLocker lock(&m_mutex);
    const auto it = m_sinks.constFind(streamId);
    return it != m_sinks.cend() && it->data() == sink;
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
