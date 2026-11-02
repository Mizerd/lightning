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

    struct Summary {
        QtMsgType type = QtWarningMsg;
        QByteArray category;
        QString text;
    };

    struct Decision {
        QList<Summary> summaries;
        bool pass = true;
    };

    RepeatLogLimiter();
    explicit RepeatLogLimiter(Limits limits);

    Decision admit(QtMsgType type, const char *category, const QString &message,
                   qint64 nowMs);

    QList<Summary> takePending(qint64 nowMs);

    quint64 suppressedTotal() const;

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
        QString message;
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

}
