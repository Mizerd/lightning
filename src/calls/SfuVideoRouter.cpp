#include "calls/SfuVideoRouter.h"

#include <QMutexLocker>
#include <QVideoSink>

SfuVideoRouter::SfuVideoRouter(QObject *parent) : QObject(parent) {}

void SfuVideoRouter::attachSink(const QString &streamId, QVideoSink *sink)
{
    if (streamId.isEmpty())
        return;
    if (!sink) {
        // A no-op, not a removal: "I have no sink yet" must not evict a
        // working surface.
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
        // Also sweep dead entries left by surfaces destroyed without a
        // release.
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
    m_pending.clear();
}

bool SfuVideoRouter::watching(const QString &streamId) const
{
    QMutexLocker lock(&m_mutex);
    const auto it = m_sinks.constFind(streamId);
    // A nulled QPointer is not a watcher; otherwise the engine would copy
    // frames for a destroyed tile.
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
            // The tile went away; drop the stale entry.
            m_sinks.remove(streamId);
            return;
        }
    }
    // Outside the lock: setVideoFrame reaches the render path, and holding a
    // mutex the streaming thread wants would stall the pipeline.
    sink->setVideoFrame(frame);
}

bool SfuVideoRouter::offerFrame(const QString &streamId,
                                const QVideoFrame &frame)
{
    QMutexLocker lock(&m_mutex);
    const bool scheduled = m_pending.contains(streamId);
    // Replaces (and so frees) a frame the GUI thread has not taken yet.
    m_pending.insert(streamId, frame);
    return !scheduled;
}

int SfuVideoRouter::pendingFramesForTest() const
{
    QMutexLocker lock(&m_mutex);
    return static_cast<int>(m_pending.size());
}

void SfuVideoRouter::deliverPending(const QString &streamId)
{
    QVideoFrame frame;
    {
        QMutexLocker lock(&m_mutex);
        const auto it = m_pending.find(streamId);
        if (it == m_pending.end())
            return;
        frame = it.value();
        // Taken: the next offer schedules the next delivery.
        m_pending.erase(it);
    }
    deliverFrame(streamId, frame);
}
