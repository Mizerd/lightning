#include "threads/ThreadManager.h"

#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QDateTime>
#include <QHash>

#include <algorithm>

ThreadManager::ThreadManager(QObject *parent)
    : QObject(parent)
{
}

void ThreadManager::setClient(MatrixClient *client)
{
    m_client = client;
}

QStringList ThreadManager::threadRootsInRoom(const QString &roomId) const
{
    if (!m_client || roomId.isEmpty())
        return {};
    QStringList roots;
    QHash<QString, bool> seen;
    for (const auto &e : m_client->timeline(roomId)) {
        if (e.threadRootId.isEmpty()) continue;
        if (!seen.contains(e.threadRootId)) {
            seen.insert(e.threadRootId, true);
            roots.append(e.threadRootId);
        }
    }
    return roots;
}

int ThreadManager::threadReplyCount(const QString &roomId,
                                    const QString &rootEventId) const
{
    if (!m_client || roomId.isEmpty() || rootEventId.isEmpty())
        return 0;
    int count = 0;
    for (const auto &e : m_client->timeline(roomId)) {
        if (e.threadRootId == rootEventId)
            ++count;
    }
    return count;
}

QVariantList ThreadManager::threadSummaries(const QString &roomId) const
{
    if (!m_client || roomId.isEmpty())
        return {};

    struct Agg {
        QString rootId;
        int count = 0;
        QDateTime lastTs;
    };
    QHash<QString, Agg> aggs;
    QHash<QString, QString> rootBodies;

    const auto events = m_client->timeline(roomId);
    for (const auto &e : events) {
        rootBodies.insert(e.eventId, e.body);
    }
    for (const auto &e : events) {
        if (e.threadRootId.isEmpty()) continue;
        Agg &a = aggs[e.threadRootId];
        a.rootId = e.threadRootId;
        a.count += 1;
        if (!e.timestamp.isValid()) continue;
        if (!a.lastTs.isValid() || e.timestamp > a.lastTs)
            a.lastTs = e.timestamp;
    }

    QVariantList out;
    for (auto it = aggs.constBegin(); it != aggs.constEnd(); ++it) {
        QVariantMap m;
        m.insert(QStringLiteral("eventId"), it->rootId);
        m.insert(QStringLiteral("preview"), rootBodies.value(it->rootId));
        m.insert(QStringLiteral("replyCount"), it->count);
        m.insert(QStringLiteral("lastReplyTs"), it->lastTs);
        out.append(m);
    }
    // Sort by lastReplyTs desc.
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("lastReplyTs")).toDateTime()
             > b.toMap().value(QStringLiteral("lastReplyTs")).toDateTime();
    });
    return out;
}
