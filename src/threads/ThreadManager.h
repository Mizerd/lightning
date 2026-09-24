#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// Thin helper for thread aggregations that need to scan several timeline
// events. Not a model: the timeline is the source of truth and the UI reads
// it directly.
//
// A thread root is an event with replies whose
// TimelineEvent::threadRootId == root.eventId (`m.thread` relation).
class ThreadManager : public QObject
{
    Q_OBJECT

public:
    explicit ThreadManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    // Thread root event ids in a room, in first-seen order.
    Q_INVOKABLE QStringList threadRootsInRoom(const QString &roomId) const;

    // Replies to a root in the currently loaded timeline. Best-effort; the
    // server aggregation may report more.
    Q_INVOKABLE int threadReplyCount(const QString &roomId,
                                     const QString &rootEventId) const;

    // [{ eventId, preview, replyCount, lastReplyTs }] sorted by lastReplyTs
    // descending.
    Q_INVOKABLE QVariantList threadSummaries(const QString &roomId) const;

    // Real thread participants, cached per (room, root).
    //
    // participants() is a pure read, safe in a QML binding.
    // requestParticipants() fetches, and is a no-op for a root that is
    // cached, in flight or queued, so cards may call it on every appearance.
    // The cache is dropped on sign-out, account switch and room change so one
    // account's faces never appear under another's.
    //
    // An empty list means "not known yet", not "nobody"; failures are not
    // distinguished, since an empty facepile beats a wrong one. At most
    // kMaxConcurrentParticipantFetches run at once; the rest wait in a FIFO.
    Q_INVOKABLE QVariantList participants(const QString &roomId,
                                          const QString &rootEventId) const;
    Q_INVOKABLE void requestParticipants(const QString &roomId,
                                         const QString &rootEventId);

    // The active room changed. Queued work for other rooms is discarded;
    // in-flight requests keep running, since their answers are keyed by room.
    // Driven by AppController, not QML.
    void setActiveRoom(const QString &roomId);

    // Test seam: how many participant fetches are in flight / queued.
    int participantFetchesInFlightForTest() const
    { return static_cast<int>(m_participantsInFlight.size()); }
    int participantFetchesQueuedForTest() const
    { return static_cast<int>(m_participantQueue.size()); }

Q_SIGNALS:
    // A root's participants arrived or changed. No payload, so QML re-reads
    // participants() and a stale binding cannot capture one.
    void participantsChanged(const QString &roomId, const QString &rootEventId);

private:
    // Releases a root whose request never answers. Above the SDK's own
    // request timeout plus retry; tripping it only allows a refetch, it never
    // caches a result.
    static constexpr int kParticipantRequestTimeoutMs = 60000;
    // Each fetch is a cache-first `load_or_fetch_event_with_relations`, a
    // paginated `/relations` chain on a miss. Keeps visible facepiles filling
    // without a request storm in rooms with many roots.
    static constexpr int kMaxConcurrentParticipantFetches = 4;
    // Backstop on the waiting queue. Excess roots are dropped, not queued;
    // scrolling their cards back into view requests them again.
    static constexpr int kMaxQueuedParticipantFetches = 64;

    static QString participantKey(const QString &roomId,
                                  const QString &rootEventId);
    static QString roomOfParticipantKey(const QString &key);
    void clearParticipants();
    // Start as many queued fetches as the concurrency bound allows.
    void pumpParticipantQueue();
    // Dispatch one key now, claiming a slot. The backend request is
    // fire-and-forget, so the slot is released by the answer or the timeout.
    void dispatchParticipants(const QString &key);
    // Release a slot and advance the queue. `generation` guards the timeout
    // path so a stale timer cannot release a later dispatch's slot; 0 means
    // unconditional (the answer path).
    void releaseParticipantSlot(const QString &key, quint64 generation = 0);

    MatrixClient *m_client = nullptr;
    // key = roomId + '\x1f' + rootEventId (the timeline ids' unit separator,
    // which neither component can contain).
    QHash<QString, QVariantList> m_participants;
    QSet<QString> m_participantsInFlight;
    // Dispatch generation per in-flight key, for stale-timeout detection.
    QHash<QString, quint64> m_participantGeneration;
    quint64 m_nextParticipantGeneration = 1;
    // FIFO of keys waiting for a slot; m_participantQueued mirrors it as a
    // set for O(1) idempotence checks.
    QStringList m_participantQueue;
    QSet<QString> m_participantQueued;
    QString m_activeRoomId;
};
