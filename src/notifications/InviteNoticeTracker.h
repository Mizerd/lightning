#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

class InviteNoticeTracker
{
public:
    static constexpr qint64 kSettleMs = 1000;
    static constexpr qint64 kGoneGraceMs = 3000;

    /// One room-list change: the ids listed as invites now. An id first seen
    /// once initial sync is done is due for announcement after kSettleMs;
    /// one seen before is recorded silently, so a restart never re-announces.
    void observe(const QSet<QString> &invites, bool initialSyncDone,
                 qint64 nowMs);
    QStringList takeDue(qint64 nowMs);
    QStringList takeResolved(qint64 nowMs);
    bool forget(const QString &id);
    qint64 nextDueInMs(qint64 nowMs) const;
    void clear();

private:
    QSet<QString> m_known;
    QHash<QString, qint64> m_goneSince;
    QHash<QString, qint64> m_dueAt;
};
