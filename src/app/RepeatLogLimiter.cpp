#include "app/RepeatLogLimiter.h"

#include <QHash>

namespace lightning::logging {

RepeatLogLimiter::RepeatLogLimiter() : RepeatLogLimiter(Limits{}) {}

RepeatLogLimiter::RepeatLogLimiter(Limits limits) : m_limits(limits)
{
    m_limits.burst = qMax(1, m_limits.burst);
    m_limits.windowMs = qMax<qint64>(1, m_limits.windowMs);
    m_limits.summaryEveryMs = qMax<qint64>(1, m_limits.summaryEveryMs);
}

QString RepeatLogLimiter::summaryText(quint64 count, qint64 spanMs,
                                      const QString &message)
{
    QString quoted = message.left(kQuoteChars);
    if (message.size() > kQuoteChars) {
        // Never half of a surrogate pair.
        if (quoted.back().isHighSurrogate())
            quoted.chop(1);
        quoted += QStringLiteral("...");
    }
    return QStringLiteral("message repeated %1 times in %2 s, not logged: %3")
        .arg(count)
        .arg(double(qMax<qint64>(0, spanMs)) / 1000.0, 0, 'f', 1)
        .arg(quoted);
}

RepeatLogLimiter::Summary RepeatLogLimiter::takeSummary(Slot &slot,
                                                        qint64 nowMs)
{
    Summary summary;
    summary.type = slot.type;
    summary.category = slot.category;
    summary.text = summaryText(slot.suppressed, nowMs - slot.suppressedSinceMs,
                               slot.message);
    slot.suppressed = 0;
    slot.suppressedSinceMs = nowMs;
    slot.lastSummaryMs = nowMs;
    return summary;
}

RepeatLogLimiter::Decision RepeatLogLimiter::admit(QtMsgType type,
                                                   const char *category,
                                                   const QString &message,
                                                   qint64 nowMs)
{
    const char *categoryName = category ? category : "";
    const size_t key = qHash(
        message, qHash(QByteArrayView(categoryName), size_t(type) + 1));

    Decision decision;
    std::lock_guard<std::mutex> lock(m_mutex);

    Slot *slot = nullptr;
    for (Slot &candidate : m_slots) {
        if (candidate.used && candidate.key == key && candidate.type == type
            && candidate.message == message
            && qstrcmp(candidate.category.constData(), categoryName) == 0) {
            slot = &candidate;
            break;
        }
    }

    // Other floods that have gone quiet: say what they cost, now.
    for (Slot &other : m_slots) {
        if (&other == slot || !other.used || !other.flooding)
            continue;
        if (nowMs - other.lastSeenMs >= m_limits.windowMs) {
            if (other.suppressed > 0)
                decision.summaries.append(takeSummary(other, nowMs));
            other.flooding = false;
        }
    }

    if (!slot) {
        // A free slot, else the one seen longest ago.
        for (Slot &candidate : m_slots) {
            if (!candidate.used) {
                slot = &candidate;
                break;
            }
            if (!slot || candidate.lastSeenMs < slot->lastSeenMs)
                slot = &candidate;
        }
        if (slot->used && slot->suppressed > 0)
            decision.summaries.append(takeSummary(*slot, nowMs));
        *slot = Slot{};
        slot->used = true;
        slot->key = key;
        slot->type = type;
        slot->category = QByteArray(categoryName);
        slot->message = message; // shared, not copied
        slot->windowStartMs = nowMs;
        slot->lastSeenMs = nowMs;
    }

    if (slot->flooding && nowMs - slot->lastSeenMs >= m_limits.windowMs) {
        // Quiet for a whole window: that flood is over.
        if (slot->suppressed > 0)
            decision.summaries.append(takeSummary(*slot, nowMs));
        slot->flooding = false;
        slot->windowStartMs = nowMs;
        slot->inWindow = 0;
    }
    slot->lastSeenMs = nowMs;

    if (!slot->flooding) {
        if (nowMs - slot->windowStartMs >= m_limits.windowMs) {
            slot->windowStartMs = nowMs;
            slot->inWindow = 0;
        }
        if (++slot->inWindow <= m_limits.burst) {
            decision.pass = true;
            return decision;
        }
        slot->flooding = true;
        slot->suppressed = 0;
        slot->suppressedSinceMs = nowMs;
        slot->lastSummaryMs = nowMs;
    }

    ++slot->suppressed;
    ++m_suppressedTotal;
    decision.pass = false;
    if (nowMs - slot->lastSummaryMs >= m_limits.summaryEveryMs)
        decision.summaries.append(takeSummary(*slot, nowMs));
    return decision;
}

QList<RepeatLogLimiter::Summary> RepeatLogLimiter::takePending(qint64 nowMs)
{
    QList<Summary> summaries;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Slot &slot : m_slots) {
        if (slot.used && slot.suppressed > 0)
            summaries.append(takeSummary(slot, nowMs));
    }
    return summaries;
}

quint64 RepeatLogLimiter::suppressedTotal() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_suppressedTotal;
}

} // namespace lightning::logging
