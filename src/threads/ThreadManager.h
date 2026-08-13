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
    Q_INVOKABLE QVariantList participants(const QString &roomId,
                                          const QString &rootEventId) const;
    Q_INVOKABLE void requestParticipants(const QString &roomId,
                                         const QString &rootEventId);

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

    static QString participantKey(const QString &roomId,
                                  const QString &rootEventId);
    void clearParticipants();

    MatrixClient *m_client = nullptr;
    // key = roomId + '\x1f' + rootEventId (the same unit separator the
    // timeline ids use; neither component can contain it).
    QHash<QString, QVariantList> m_participants;
    QSet<QString> m_participantsInFlight;
};
