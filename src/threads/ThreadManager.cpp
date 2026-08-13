#include "threads/ThreadManager.h"

#include "matrix/MatrixClient.h"
#include "matrix/TimelineEvent.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QTimer>

#include <algorithm>

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
        m_participantsInFlight.remove(key);
        // A failed lookup arrives empty. Do NOT cache it: caching would
        // turn a transient failure into a permanent "no participants" for
        // the rest of the session, and the next request would be
        // suppressed as already-known. Leaving it uncached lets the card
        // simply try again.
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

void ThreadManager::clearParticipants()
{
    m_participants.clear();
    m_participantsInFlight.clear();
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
    // Idempotent: already known, or already asked. This is what makes it
    // safe for every visible summary card to call on every appearance —
    // without it, scrolling a timeline full of thread roots would issue a
    // request per card per scroll.
    if (m_participants.contains(key) || m_participantsInFlight.contains(key))
        return;
    m_participantsInFlight.insert(key);
    m_client->requestThreadParticipants(roomId, rootEventId);
    // Several paths never answer at all: the backend refuses while logged
    // out, the Rust side returns Err before spawning (unknown room, left
    // room, unparsable root), the spawned task drops on a lifecycle change,
    // or the event queue overflows. Without a timeout the key would stay
    // in-flight forever and that root could never be retried for the rest
    // of the session — a silent permanent "no facepile". The timeout only
    // releases the key; it never caches a result, so the next request is a
    // genuine retry.
    const QPointer<ThreadManager> guard(this);
    QTimer::singleShot(kParticipantRequestTimeoutMs, this, [guard, key] {
        if (!guard)
            return;
        guard->m_participantsInFlight.remove(key);
    });
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
