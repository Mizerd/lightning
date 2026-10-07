// Bounds what one message repeated without end can cost the user's disk.
//
// Measured 2026-10-07 (Flatpak, Qt 6.11.2): a Qt event dispatcher looping on
// a closed file descriptor logged "QSocketNotifier: Invalid socket 124 and
// type 'Read', disabling..." about 95 million times in 7 minutes. --log-file
// and stderr each grew to 9.8 GB and filled the guest's disk. The log sinks
// already never block the caller (AsyncLogSink); nothing bounded how much an
// identical line could write.
//
// Per distinct message (type, category and text): the first `burst` within a
// window pass. Past that the message is suppressed until it has been quiet
// for a whole window, and while it keeps coming one summary line says how
// many were suppressed, at most once per `summaryEveryMs`. When it stops, the
// remainder is summarised the next time anything is logged, or at exit
// (takePending). A runaway thus costs `burst` lines plus one line per
// interval, for as long as it runs.
//
// The summary repeats the message (shortened) and nothing else, so it carries
// nothing the line itself did not; message bodies, keys and tokens are never
// logged in the first place (CLAUDE.md §6).
//
// Thread-safe: Qt calls message handlers on any thread. One short mutex per
// message; nothing allocates for a suppressed message.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <array>
#include <mutex>

namespace lightning::logging {

class RepeatLogLimiter
{
public:
    struct Limits {
        int burst = 20;
        qint64 windowMs = 1000;
        qint64 summaryEveryMs = 10000;
    };

    /// A line to log in place of what was suppressed, with the type and
    /// category of the message it summarises.
    struct Summary {
        QtMsgType type = QtWarningMsg;
        QByteArray category;
        QString text;
    };

    struct Decision {
        /// Log these first (in order); they may concern other messages.
        QList<Summary> summaries;
        /// Whether the message itself is logged.
        bool pass = true;
    };

    RepeatLogLimiter();
    explicit RepeatLogLimiter(Limits limits);

    /// `nowMs` is a monotonic clock in milliseconds.
    Decision admit(QtMsgType type, const char *category, const QString &message,
                   qint64 nowMs);

    /// Summaries for everything suppressed and not yet reported, for the
    /// process exit: a flood that ends with the process is still accounted.
    QList<Summary> takePending(qint64 nowMs);

    /// Messages suppressed since construction.
    quint64 suppressedTotal() const;

    /// The summary line for `count` suppressed copies of `message` over
    /// `spanMs`; exposed for tests.
    static QString summaryText(quint64 count, qint64 spanMs,
                               const QString &message);

    static constexpr int kSlots = 16;
    static constexpr int kQuoteChars = 200;

private:
    struct Slot {
        size_t key = 0;
        bool used = false;
        QtMsgType type = QtDebugMsg;
        QByteArray category;
        QString message; // kept only while suppressing, for the summary
        qint64 windowStartMs = 0;
        int inWindow = 0;
        qint64 lastSeenMs = 0;
        bool flooding = false;
        quint64 suppressed = 0;
        qint64 suppressedSinceMs = 0;
        qint64 lastSummaryMs = 0;
    };

    static Summary takeSummary(Slot &slot, qint64 nowMs);

    Limits m_limits;
    mutable std::mutex m_mutex;
    std::array<Slot, kSlots> m_slots;
    quint64 m_suppressedTotal = 0;
};

} // namespace lightning::logging
