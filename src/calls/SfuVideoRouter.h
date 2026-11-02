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

    Q_INVOKABLE void attachSink(const QString &streamId, QVideoSink *sink);

    void releaseSink(QVideoSink *sink);

    /// Forget every sink, on teardown. There is intentionally no per-key
    /// removal: it would let a dying tile unhook its replacement.
    void clear();

    bool watching(const QString &streamId) const;

    bool watchedBy(const QString &streamId, const QVideoSink *sink) const;

    bool offerFrame(const QString &streamId, const QVideoFrame &frame);

    int pendingFramesForTest() const;

public Q_SLOTS:
    void deliverFrame(const QString &streamId, const QVideoFrame &frame);

    void deliverPending(const QString &streamId);

private:
    mutable QMutex m_mutex;
    QHash<QString, QPointer<QVideoSink>> m_sinks;
    QHash<QString, QVideoFrame> m_pending;
};
