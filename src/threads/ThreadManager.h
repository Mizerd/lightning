#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

// v0.4.1: thin helper around the timeline for thread-related queries. Not
// a QAbstractListModel — the timeline is the source of truth, so threading
// UI reads it directly and calls back here only for aggregations that
// require scanning multiple events.
//
// A "thread root" is an event that has one or more replies where
// TimelineEvent::threadRootId == root.eventId (Matrix `m.thread` relation).
// Sending a thread reply is done via MatrixClient::sendReply with the root
// event id (v0.4.1 uses reply-style; real m.thread relation content is a
// v0.5 follow-up documented in docs/matrix-feature-status.md).
class ThreadManager : public QObject
{
    Q_OBJECT

public:
    explicit ThreadManager(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    // Return the list of thread root event ids present in a room (in order
    // of first-seen). Rooms with no threads return an empty list.
    Q_INVOKABLE QStringList threadRootsInRoom(const QString &roomId) const;

    // Return the number of replies attached to a thread root by scanning
    // the room's currently-loaded timeline. Best-effort — server-side
    // aggregation (unsigned["m.relations"]) may report higher counts than
    // what's locally visible.
    Q_INVOKABLE int threadReplyCount(const QString &roomId,
                                     const QString &rootEventId) const;

    // Convenience: [{ eventId, preview, replyCount, lastReplyTs }] sorted
    // by lastReplyTs descending. Useful for a thread-list side panel.
    Q_INVOKABLE QVariantList threadSummaries(const QString &roomId) const;

    // v0.7 facepiles: REAL thread participants, cached per (room, root).
    //
    // participants() is a pure read — it never issues a request, so it is
    // safe to call from a QML binding. requestParticipants() is the
    // explicit fetch, and it is idempotent per root: a root that is already
    // cached or already in flight costs nothing, so a summary card may call
    // it every time it becomes visible without generating traffic. Both are
    // scoped by room id, and the whole cache is dropped on sign-out, account
    // switch and room change so one account's faces can never appear under
    // another's.
    //
    // An empty list means "not known yet", NOT "nobody" — a failed lookup is
    // indistinguishable from a pending one by design, because rendering an
    // empty facepile is preferable to rendering a wrong one.
    // Fan-out is BOUNDED (v0.7.x): the room timeline is not virtualized, so
    // every loaded thread root's card calls requestParticipants on the same
    // frame — before this, opening a thread-heavy room dispatched one
    // `/relations` chain per root at once. At most
    // kMaxConcurrentParticipantFetches are in flight; the rest wait in a
    // FIFO queue and start as slots free. Nothing is dropped, only paced.
    Q_INVOKABLE QVariantList participants(const QString &roomId,
                                          const QString &rootEventId) const;
    Q_INVOKABLE void requestParticipants(const QString &roomId,
                                         const QString &rootEventId);

    // The active room changed. QUEUED work for other rooms is discarded —
    // those cards are gone and their answers would only compete with the
    // new room's. In-flight requests are left alone: they cannot contaminate
    // the new room (the cache key carries the room id) and cancelling them
    // would waste a fetch that is already paid for. Not a QML entry point;
    // AppController drives it.
    void setActiveRoom(const QString &roomId);

    // Test seam: how many participant fetches are in flight / queued.
    int participantFetchesInFlightForTest() const
    { return static_cast<int>(m_participantsInFlight.size()); }
    int participantFetchesQueuedForTest() const
    { return static_cast<int>(m_participantQueue.size()); }

Q_SIGNALS:
    // A root's participants arrived (or changed). QML re-reads
    // participants() for that root; no payload travels with the signal so a
    // stale binding cannot capture one.
    void participantsChanged(const QString &roomId, const QString &rootEventId);

private:
    // A request that never answers must not block that root forever; see
    // requestParticipants for the paths that answer nothing at all. Sits
    // comfortably above the SDK's own per-request timeout plus retry, so a
    // slow-but-successful fetch rarely trips it. Tripping it is cheap
    // anyway: the key is released without caching a result, so the worst
    // case is one redundant (cache-first) refetch, never a wrong answer.
    static constexpr int kParticipantRequestTimeoutMs = 60000;
    // Concurrency bound. Each fetch is a cache-first
    // `load_or_fetch_event_with_relations`, which on a miss is a paginated
    // `/relations` chain — cheap when cached, a real request otherwise.
    // Four keeps a visible facepile filling promptly while a room with
    // dozens of loaded roots cannot become a request storm.
    static constexpr int kMaxConcurrentParticipantFetches = 4;
    // Backstop on the waiting queue. A room with more loaded thread roots
    // than this has more facepiles than a reader can look at; the excess is
    // dropped rather than queued indefinitely, and scrolling those cards
    // back into view re-requests them (requestParticipants is idempotent
    // only for cached/in-flight/queued roots, never for dropped ones).
    static constexpr int kMaxQueuedParticipantFetches = 64;

    static QString participantKey(const QString &roomId,
                                  const QString &rootEventId);
    static QString roomOfParticipantKey(const QString &key);
    void clearParticipants();
    // Start as many queued fetches as the concurrency bound allows.
    void pumpParticipantQueue();
    // Dispatch one key immediately, claiming a concurrency slot. The
    // backend's request is fire-and-forget (it reports no synchronous
    // refusal), so the slot is released by the answer or by the timeout —
    // never assumed.
    void dispatchParticipants(const QString &key);
    // Release a slot and let the queue advance. `generation` guards the
    // TIMEOUT path: a key can be dispatched again after its first answer, so
    // a stale 60 s timer must not release the SECOND dispatch's slot (which
    // would admit past the cap). 0 means "unconditional" — the answer path,
    // which is always about the current dispatch.
    void releaseParticipantSlot(const QString &key, quint64 generation = 0);

    MatrixClient *m_client = nullptr;
    // key = roomId + '\x1f' + rootEventId (the same unit separator the
    // timeline ids use; neither component can contain it).
    QHash<QString, QVariantList> m_participants;
    QSet<QString> m_participantsInFlight;
    // Which dispatch each in-flight key belongs to, so a stale timeout can
    // recognise that it is no longer the current one.
    QHash<QString, quint64> m_participantGeneration;
    quint64 m_nextParticipantGeneration = 1;
    // FIFO of keys waiting for a slot. m_participantQueued mirrors it as a
    // set so the idempotence check stays O(1) — a card that becomes visible
    // repeatedly while queued must not enqueue itself again.
    QStringList m_participantQueue;
    QSet<QString> m_participantQueued;
    QString m_activeRoomId;
};
