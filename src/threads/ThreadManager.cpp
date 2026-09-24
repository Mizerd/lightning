#include "threads/ThreadManager.h"

#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

ThreadManager::ThreadManager(QObject *parent)
    : QObject(parent)
{
}

void ThreadManager::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    clearParticipants();
    m_client = client;
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::threadParticipantsReceived, this,
            [this](const QString &roomId, const QString &rootEventId,
                   const QVariantList &participants, int distinct,
                   bool truncated) {
        Q_UNUSED(distinct);
        Q_UNUSED(truncated);
        const QString key = participantKey(roomId, rootEventId);
        // Free the slot first so the queue advances even on failure.
        releaseParticipantSlot(key);
        // A failed lookup arrives empty. Do not cache it, or a transient
        // failure would become a permanent "no participants" for the session.
        if (participants.isEmpty())
            return;
        m_participants.insert(key, participants);
        Q_EMIT participantsChanged(roomId, rootEventId);
    });
    // One account's faces must never be shown under another's.
    connect(m_client, &MatrixClient::loggedOut, this,
            [this] { clearParticipants(); });
}

QString ThreadManager::participantKey(const QString &roomId,
                                      const QString &rootEventId)
{
    return roomId + QChar(0x1F) + rootEventId;
}

QString ThreadManager::roomOfParticipantKey(const QString &key)
{
    const int sep = key.indexOf(QChar(0x1F));
    return sep < 0 ? QString() : key.left(sep);
}

void ThreadManager::clearParticipants()
{
    m_participants.clear();
    m_participantsInFlight.clear();
    m_participantGeneration.clear();
    m_participantQueue.clear();
    m_participantQueued.clear();
}

void ThreadManager::setActiveRoom(const QString &roomId)
{
    if (m_activeRoomId == roomId)
        return;
    m_activeRoomId = roomId;
    // Discard queued work for other rooms. In-flight requests keep running:
    // their answers are keyed by room and cancelling would waste the fetch.
    if (m_participantQueue.isEmpty())
        return;
    QStringList kept;
    kept.reserve(m_participantQueue.size());
    for (const QString &key : std::as_const(m_participantQueue)) {
        if (roomOfParticipantKey(key) == roomId)
            kept.append(key);
        else
            m_participantQueued.remove(key);
    }
    m_participantQueue = kept;
    pumpParticipantQueue();
}

void ThreadManager::dispatchParticipants(const QString &key)
{
    const int sep = key.indexOf(QChar(0x1F));
    if (!m_client || sep < 0)
        return;
    const QString roomId = key.left(sep);
    const QString rootEventId = key.mid(sep + 1);
    const quint64 generation = m_nextParticipantGeneration++;
    m_participantsInFlight.insert(key);
    m_participantGeneration.insert(key, generation);
    m_client->requestThreadParticipants(roomId, rootEventId);
    // Some paths never answer (logged out, Err before spawning, task dropped
    // on a lifecycle change, event queue overflow). Without a timeout the key
    // would hold a slot forever. The timeout only releases the key; it never
    // caches a result.
    const QPointer<ThreadManager> guard(this);
    QTimer::singleShot(kParticipantRequestTimeoutMs, this,
                       [guard, key, generation] {
        if (!guard)
            return;
        guard->releaseParticipantSlot(key, generation);
    });
}

void ThreadManager::releaseParticipantSlot(const QString &key,
                                           quint64 generation)
{
    if (!m_participantsInFlight.contains(key))
        return; // already released (answer beat the timeout, or vice versa)
    // A failed lookup is not cached, so the root can be redispatched before
    // the first timer fires; that stale timer must not release the new slot.
    if (generation != 0
        && m_participantGeneration.value(key, 0) != generation) {
        return;
    }
    m_participantsInFlight.remove(key);
    m_participantGeneration.remove(key);
    pumpParticipantQueue();
}

void ThreadManager::pumpParticipantQueue()
{
    while (!m_participantQueue.isEmpty()
           && m_participantsInFlight.size()
                  < kMaxConcurrentParticipantFetches) {
        const QString key = m_participantQueue.takeFirst();
        m_participantQueued.remove(key);
        // It may have been answered while waiting; skip rather than re-fetch.
        if (m_participants.contains(key)
            || m_participantsInFlight.contains(key)) {
            continue;
        }
        dispatchParticipants(key);
    }
}

QVariantList ThreadManager::participants(const QString &roomId,
                                         const QString &rootEventId) const
{
    if (roomId.isEmpty() || rootEventId.isEmpty())
        return {};
    return m_participants.value(participantKey(roomId, rootEventId));
}

void ThreadManager::requestParticipants(const QString &roomId,
                                        const QString &rootEventId)
{
    if (!m_client || roomId.isEmpty() || rootEventId.isEmpty())
        return;
    const QString key = participantKey(roomId, rootEventId);
    // Idempotent: already known, in flight, or queued. Lets every visible
    // summary card call this on each appearance.
    if (m_participants.contains(key) || m_participantsInFlight.contains(key)
        || m_participantQueued.contains(key)) {
        return;
    }
    // Dispatch under the bound, otherwise queue: the timeline is not
    // virtualized, so a thread-heavy room asks for every root on one frame.
    if (m_participantsInFlight.size() < kMaxConcurrentParticipantFetches) {
        dispatchParticipants(key);
        return;
    }
    if (m_participantQueue.size() >= kMaxQueuedParticipantFetches)
        return; // dropped, and therefore genuinely retryable later
    m_participantQueue.append(key);
    m_participantQueued.insert(key);
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
