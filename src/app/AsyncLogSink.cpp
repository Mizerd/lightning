#include "AsyncLogSink.h"

#include <QDateTime>
#include <QFileDevice>
#include <QIODevice>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <utility>
#include <vector>

namespace lightning::logging {

namespace {

// Set on the writer thread. A line logged there (a device that warns about
// its own write) would feed the queue it is draining, so it is not queued.
thread_local bool t_onWriterThread = false;

std::atomic<AsyncLogSink *> g_processSink{nullptr};

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

std::chrono::milliseconds boundOf(int timeoutMs)
{
    return std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
}

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "debug";
    case QtInfoMsg:     return "info";
    case QtWarningMsg:  return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg:    return "fatal";
    }
    return "info";
}

// "<level> <category>: <message>\n"; the stamp goes in front of it.
QByteArray lineBody(QtMsgType type, const char *category,
                    const QString &message)
{
    const char *level = levelName(type);
    const char *name = category ? category : "default";
    const QByteArray text = message.toUtf8();
    QByteArray body;
    body.reserve(qsizetype(qstrlen(level) + qstrlen(name)) + text.size() + 4);
    body += level;
    body += ' ';
    body += name;
    body += ": ";
    body += text;
    body += '\n';
    return body;
}

void appendNumber(QByteArray &out, qint64 value, int width)
{
    char digits[24];
    int n = 0;
    do {
        digits[n++] = char('0' + value % 10);
        value /= 10;
    } while (value > 0 && n < int(sizeof digits));
    for (int pad = width - n; pad > 0; --pad)
        out += '0';
    while (n > 0)
        out += digits[--n];
}

void writeAndFlush(QIODevice &device, const QByteArray &bytes)
{
    if (!bytes.isEmpty())
        device.write(bytes);
    // A plain QIODevice has nothing to flush; a file has Qt's buffer.
    if (auto *file = qobject_cast<QFileDevice *>(&device))
        file->flush();
}

} // namespace

struct AsyncLogSink::State {
    struct Entry {
        qint64 whenMs;
        QByteArray text;
        bool stamp;
    };

    explicit State(std::unique_ptr<QIODevice> d, Limits l)
        : limits(l), device(std::move(d)) {}

    const Limits limits;
    // Written by the writer thread; once it has exited, by a fatal line under
    // directWriting.
    const std::unique_ptr<QIODevice> device;

    // Guards everything below. Held for queue operations only, never across
    // a write to the device.
    std::mutex mutex;
    std::condition_variable wake;       // the writer: work or stop
    std::condition_variable progress;   // written, or the writer exited
    std::vector<Entry> queue;
    qint64 queuedBytes = 0;
    // Drops since the writer last took the queue. While non-zero nothing is
    // queued, so the notice lands exactly where the gap is.
    quint64 dropped = 0;
    qint64 firstDropMs = 0;
    qint64 lastDropMs = 0;
    quint64 droppedTotal = 0;
    // Sequence of the last accepted line (a gap's notice takes one too), and
    // of the last one written and flushed.
    quint64 accepted = 0;
    quint64 written = 0;
    bool directWriting = false;
    bool closed = false;
    bool stopping = false;
    bool exited = false;
};

namespace {

// The line owed for a gap, stamped with its first drop.
AsyncLogSink::State::Entry dropNotice(quint64 dropped, qint64 firstMs,
                                      qint64 lastMs)
{
    QByteArray body = "warning lightning.log: dropped ";
    body += QByteArray::number(dropped);
    body += " log lines logged between ";
    body += AsyncLogSink::isoTimestamp(firstMs);
    body += " and ";
    body += AsyncLogSink::isoTimestamp(lastMs);
    body += ": the log file was not accepting writes\n";
    return {firstMs, body, true};
}

void appendEntry(QByteArray &out, const AsyncLogSink::State::Entry &entry)
{
    if (entry.stamp) {
        out += AsyncLogSink::isoTimestamp(entry.whenMs);
        out += ' ';
    }
    out += entry.text;
}

void runWriter(const std::shared_ptr<AsyncLogSink::State> &state)
{
    AsyncLogSink::State &s = *state;
    t_onWriterThread = true;
    std::vector<AsyncLogSink::State::Entry> batch;
    QByteArray out;
    for (;;) {
        quint64 dropped = 0;
        qint64 firstDropMs = 0;
        qint64 lastDropMs = 0;
        quint64 target = 0;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.wake.wait(lock, [&s] {
                return !s.queue.empty() || s.dropped > 0 || s.stopping;
            });
            if (s.queue.empty() && s.dropped == 0) {
                s.exited = true;
                break;
            }
            batch.swap(s.queue);
            s.queuedBytes = 0;
            dropped = std::exchange(s.dropped, quint64(0));
            firstDropMs = s.firstDropMs;
            lastDropMs = s.lastDropMs;
            target = s.accepted;
        }

        out.clear();
        for (const AsyncLogSink::State::Entry &entry : batch)
            appendEntry(out, entry);
        batch.clear();
        // Everything in the batch was queued before the first drop.
        if (dropped > 0)
            appendEntry(out, dropNotice(dropped, firstDropMs, lastDropMs));
        // One write and one flush per batch.
        writeAndFlush(*s.device, out);
        if (out.capacity() > (1 << 20))
            out = QByteArray();

        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.written = target;
        }
        s.progress.notify_all();
    }
    s.progress.notify_all();
}

} // namespace

AsyncLogSink::AsyncLogSink(std::unique_ptr<QIODevice> device)
    : AsyncLogSink(std::move(device), Limits{})
{
}

AsyncLogSink::AsyncLogSink(std::unique_ptr<QIODevice> device, Limits limits)
    : m_state(std::make_shared<State>(std::move(device), limits))
{
    // The thread holds the state, not the sink: a writer left behind by
    // shutdown() outlives this object.
    m_writer = std::thread([state = m_state] { runWriter(state); });
}

AsyncLogSink::~AsyncLogSink()
{
    shutdown(kShutdownMs);
}

bool AsyncLogSink::log(QtMsgType type, const char *category,
                       const QString &message)
{
    // Stamped before anything can wait, so a line keeps the time it was
    // logged even when it reaches the file much later.
    const qint64 whenMs = nowMs();
    return enqueue(whenMs, lineBody(type, category, message), true);
}

bool AsyncLogSink::writeVerbatim(const QString &text)
{
    if (text.isEmpty())
        return true;
    return enqueue(nowMs(), text.toUtf8(), false);
}

bool AsyncLogSink::enqueue(qint64 whenMs, QByteArray text, bool stamp)
{
    if (t_onWriterThread)
        return false;
    State &s = *m_state;
    const qint64 size = text.size();
    bool wakeWriter = false;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.closed)
            return false;
        const bool full = s.dropped > 0
            || qsizetype(s.queue.size()) >= s.limits.maxLines
            || s.queuedBytes + size > s.limits.maxBytes;
        if (full) {
            // Never wait for room: count it, and owe a notice.
            if (s.dropped == 0) {
                ++s.accepted;
                s.firstDropMs = whenMs;
                wakeWriter = s.queue.empty();
            }
            ++s.dropped;
            ++s.droppedTotal;
            s.lastDropMs = whenMs;
        } else {
            wakeWriter = s.queue.empty();
            s.queue.push_back({whenMs, std::move(text), stamp});
            s.queuedBytes += size;
            ++s.accepted;
            queued = true;
        }
    }
    // Only an idle writer waits, and it waits only on an empty queue.
    if (wakeWriter)
        s.wake.notify_one();
    return queued;
}

bool AsyncLogSink::flush(int timeoutMs)
{
    if (t_onWriterThread)
        return false;
    State &s = *m_state;
    std::unique_lock<std::mutex> lock(s.mutex);
    const quint64 target = s.accepted;
    return s.progress.wait_for(lock, boundOf(timeoutMs),
                               [&s, target] { return s.written >= target; });
}

bool AsyncLogSink::logFatal(const char *category, const QString &message,
                            int timeoutMs)
{
    const qint64 whenMs = nowMs();
    QByteArray body = lineBody(QtFatalMsg, category, message);
    if (t_onWriterThread)
        return false;
    State &s = *m_state;
    std::unique_lock<std::mutex> lock(s.mutex);
    if (s.exited && !s.directWriting) {
        // The writer is gone (static destruction): nobody else writes.
        s.directWriting = true;
        lock.unlock();
        QByteArray line = isoTimestamp(whenMs);
        line += ' ';
        line += body;
        writeAndFlush(*s.device, line);
        lock.lock();
        s.directWriting = false;
        return true;
    }
    // Queued past the bound and after the notice for any gap before it, so
    // it lands last and in order; then the writer gets the bound to reach it.
    // A direct write here would race the writer, or block where it blocks.
    if (s.dropped > 0) {
        // Its sequence number was taken at the first drop.
        s.queue.push_back(dropNotice(s.dropped, s.firstDropMs, s.lastDropMs));
        s.dropped = 0;
    }
    s.queuedBytes += body.size();
    s.queue.push_back({whenMs, std::move(body), true});
    const quint64 target = ++s.accepted;
    s.wake.notify_one();
    return s.progress.wait_for(lock, boundOf(timeoutMs),
                               [&s, target] { return s.written >= target; });
}

void AsyncLogSink::shutdown(int timeoutMs)
{
    std::lock_guard<std::mutex> lifecycle(m_lifecycle);
    if (!m_writer.joinable()
        || m_writer.get_id() == std::this_thread::get_id())
        return;
    State &s = *m_state;
    bool exited = false;
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        s.closed = true;
        s.stopping = true;
        s.wake.notify_one();
        exited = s.progress.wait_for(lock, boundOf(timeoutMs),
                                     [&s] { return s.exited; });
    }
    if (exited)
        m_writer.join();
    else
        m_writer.detach();
}

quint64 AsyncLogSink::droppedTotal() const
{
    State &s = *m_state;
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.droppedTotal;
}

QByteArray AsyncLogSink::formatLine(qint64 msSinceEpochUtc, QtMsgType type,
                                    const char *category,
                                    const QString &message)
{
    QByteArray line = isoTimestamp(msSinceEpochUtc);
    line += ' ';
    line += lineBody(type, category, message);
    return line;
}

QByteArray AsyncLogSink::isoTimestamp(qint64 msSinceEpochUtc)
{
    // A clock before 1970 is not worth a floor division.
    const qint64 ms = msSinceEpochUtc > 0 ? msSinceEpochUtc : 0;
    const qint64 days = ms / 86400000;
    const qint64 msOfDay = ms % 86400000;
    // Days to civil date (H. Hinnant's civil_from_days, days >= 0).
    const qint64 z = days + 719468;
    const qint64 era = z / 146097;
    const qint64 doe = z - era * 146097;
    const qint64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const qint64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const qint64 mp = (5 * doy + 2) / 153;
    const qint64 day = doy - (153 * mp + 2) / 5 + 1;
    const qint64 month = mp < 10 ? mp + 3 : mp - 9;
    const qint64 year = yoe + era * 400 + (month <= 2 ? 1 : 0);

    QByteArray out;
    out.reserve(24);
    appendNumber(out, year, 4);
    out += '-';
    appendNumber(out, month, 2);
    out += '-';
    appendNumber(out, day, 2);
    out += 'T';
    appendNumber(out, msOfDay / 3600000, 2);
    out += ':';
    appendNumber(out, (msOfDay / 60000) % 60, 2);
    out += ':';
    appendNumber(out, (msOfDay / 1000) % 60, 2);
    out += '.';
    appendNumber(out, msOfDay % 1000, 3);
    out += 'Z';
    return out;
}

AsyncLogSink *processLogSink()
{
    return g_processSink.load(std::memory_order_acquire);
}

void setProcessLogSink(AsyncLogSink *sink)
{
    g_processSink.store(sink, std::memory_order_release);
}

bool flushProcessLog(int timeoutMs)
{
    AsyncLogSink *sink = processLogSink();
    return !sink || sink->flush(timeoutMs);
}

} // namespace lightning::logging
