// A log destination that never blocks the thread that logs.
//
// --log-file used to write and flush the file under one global mutex on
// whichever thread logged. GStreamer streaming threads log from pad probes,
// so a log file that stopped accepting writes (a stalled pipe, btrfs
// dirty-page throttling) silenced call audio both ways and froze the GUI.
//
// A caller stamps its line with the time of the call and queues it; one
// writer thread owns the device. The queue is bounded: when it is full a
// line is dropped and counted, never waited for, and once the writer catches
// up it writes one line saying how many were lost and when. Lines keep their
// order. The writer flushes after every batch, so a crash loses as little as
// possible.
//
// Portable: std::thread and Qt only, as GuiStallTracer.
#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <memory>
#include <mutex>
#include <thread>

class QIODevice;

namespace lightning::logging {

class AsyncLogSink
{
public:
    struct Limits {
        int maxLines = 8192;
        qint64 maxBytes = 4 * 1024 * 1024;
        int coalesceMs = 0;
    };

    static constexpr int kFatalDrainMs = 1000;
    static constexpr int kShutdownMs = 2000;

    explicit AsyncLogSink(std::unique_ptr<QIODevice> device);
    AsyncLogSink(std::unique_ptr<QIODevice> device, Limits limits);
    ~AsyncLogSink();

    AsyncLogSink(const AsyncLogSink &) = delete;
    AsyncLogSink &operator=(const AsyncLogSink &) = delete;

    bool log(QtMsgType type, const char *category, const QString &message);
    bool writeVerbatim(const QString &text);

    bool flush(int timeoutMs);

    bool logFatal(const char *category, const QString &message, int timeoutMs);

    void shutdown(int timeoutMs);

    quint64 droppedTotal() const;

    static QByteArray formatLine(qint64 msSinceEpochUtc, QtMsgType type,
                                 const char *category, const QString &message);
    static QByteArray isoTimestamp(qint64 msSinceEpochUtc);

    struct State;

private:
    bool enqueue(qint64 whenMs, QByteArray text, bool stamp);

    std::shared_ptr<State> m_state;
    std::mutex m_lifecycle;
    std::thread m_writer;
};

AsyncLogSink *processLogSink();
void setProcessLogSink(AsyncLogSink *sink);
bool flushProcessLog(int timeoutMs);

}
