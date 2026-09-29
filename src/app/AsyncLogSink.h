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
        // Lines and bytes queued and not yet taken by the writer. The batch
        // being written is not counted: while it is copied out for the write,
        // up to three times this is held; while a write is stuck, twice.
        int maxLines = 8192;
        qint64 maxBytes = 4 * 1024 * 1024;
    };

    // Bounded waits used by the --log-file handler.
    static constexpr int kFatalDrainMs = 1000;
    static constexpr int kShutdownMs = 2000;

    // Takes an open, writable device and starts the writer.
    explicit AsyncLogSink(std::unique_ptr<QIODevice> device);
    AsyncLogSink(std::unique_ptr<QIODevice> device, Limits limits);
    // shutdown(kShutdownMs). Nothing may log to it any more.
    ~AsyncLogSink();

    AsyncLogSink(const AsyncLogSink &) = delete;
    AsyncLogSink &operator=(const AsyncLogSink &) = delete;

    // Queues "<UTC time> <level> <category>: <message>\n", stamped now.
    // Never waits. False when the line was dropped or the sink is shut down.
    bool log(QtMsgType type, const char *category, const QString &message);
    // Queues program output as it is (no stamp).
    bool writeVerbatim(const QString &text);

    // Waits up to timeoutMs for everything queued before the call, and any
    // drop notice owed for it, to be written and flushed.
    bool flush(int timeoutMs);

    // For QtFatalMsg, before the abort: queues the line past the bound, last,
    // and waits up to timeoutMs for the writer to drain the queue through it.
    // Once the writer thread has exited (static destruction) it writes the
    // line directly instead. True when the line is in the file; false at the bound,
    // when the writer is stuck where a direct write would block as well.
    bool logFatal(const char *category, const QString &message, int timeoutMs);

    // Stops accepting lines and gives the writer up to timeoutMs to write
    // what is queued. A writer still stuck in the device at the bound is left
    // behind (a running thread cannot be joined) and keeps the device until
    // it returns. Idempotent. Never call it from a message handler.
    void shutdown(int timeoutMs);

    // Lines dropped because the queue was full.
    quint64 droppedTotal() const;

    // The exact line log() writes for an instant; exposed for tests.
    static QByteArray formatLine(qint64 msSinceEpochUtc, QtMsgType type,
                                 const char *category, const QString &message);
    // "2026-09-29T13:46:57.876Z", as QDateTime's ISODateWithMs in UTC, but
    // plain arithmetic: the writer may still run during static destruction.
    static QByteArray isoTimestamp(qint64 msSinceEpochUtc);

    struct State;

private:
    bool enqueue(qint64 whenMs, QByteArray text, bool stamp);

    std::shared_ptr<State> m_state;
    std::mutex m_lifecycle;   // guards m_writer
    std::thread m_writer;
};

// The process-wide --log-file sink, or null. Once set it is never freed: a
// message handler can still run during static destruction, and it must find
// a sink that is shut down, not one that is gone.
AsyncLogSink *processLogSink();
void setProcessLogSink(AsyncLogSink *sink);
// Flushes the process-wide sink, if there is one; for exits that skip the
// normal teardown. True when there was nothing left to write.
bool flushProcessLog(int timeoutMs);

} // namespace lightning::logging
