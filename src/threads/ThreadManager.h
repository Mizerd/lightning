#pragma once

#include <QObject>
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

private:
    MatrixClient *m_client = nullptr;
};
