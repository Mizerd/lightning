#include "notifications/InviteNoticeTracker.h"

#include <algorithm>
#include <utility>

void InviteNoticeTracker::observe(const QSet<QString> &invites,
                                  bool initialSyncDone, qint64 nowMs)
{
    for (const QString &id : invites) {
        m_goneSince.remove(id);
        if (m_known.contains(id))
            continue;
        m_known.insert(id);
        if (initialSyncDone)
            m_dueAt.insert(id, nowMs + kSettleMs);
    }
    for (const QString &id : std::as_const(m_known)) {
        if (!invites.contains(id) && !m_goneSince.contains(id))
            m_goneSince.insert(id, nowMs);
    }
}

QStringList InviteNoticeTracker::takeDue(qint64 nowMs)
{
    QStringList due;
    for (auto it = m_dueAt.begin(); it != m_dueAt.end();) {
        // One that is off the list right now waits: it may be mid re-sort,
        // and if it has really gone, takeResolved() drops it.
        if (it.value() <= nowMs && !m_goneSince.contains(it.key())) {
            due.append(it.key());
            it = m_dueAt.erase(it);
        } else {
            ++it;
        }
    }
    due.sort();
    return due;
}

QStringList InviteNoticeTracker::takeResolved(qint64 nowMs)
{
    QStringList resolved;
    for (auto it = m_goneSince.begin(); it != m_goneSince.end();) {
        if (nowMs - it.value() >= kGoneGraceMs) {
            resolved.append(it.key());
            m_known.remove(it.key());
            m_dueAt.remove(it.key());
            it = m_goneSince.erase(it);
        } else {
            ++it;
        }
    }
    resolved.sort();
    return resolved;
}

bool InviteNoticeTracker::forget(const QString &id)
{
    m_goneSince.remove(id);
    m_dueAt.remove(id);
    return m_known.remove(id);
}

qint64 InviteNoticeTracker::nextDueInMs(qint64 nowMs) const
{
    qint64 next = -1;
    const auto consider = [&next, nowMs](qint64 at) {
        const qint64 wait = std::max<qint64>(0, at - nowMs);
        next = next < 0 ? wait : std::min(next, wait);
    };
    for (auto it = m_dueAt.cbegin(); it != m_dueAt.cend(); ++it) {
        if (!m_goneSince.contains(it.key()))
            consider(it.value());
    }
    for (auto it = m_goneSince.cbegin(); it != m_goneSince.cend(); ++it)
        consider(it.value() + kGoneGraceMs);
    return next;
}

void InviteNoticeTracker::clear()
{
    m_known.clear();
    m_goneSince.clear();
    m_dueAt.clear();
}
