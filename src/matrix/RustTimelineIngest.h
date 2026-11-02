#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace matrix::rust_timeline {

struct DiffOutcome {
    enum Kind {
        Invalid,
        Appended,
        Prepended,
        Inserted,
        Changed,
        Removed,
        Cleared,
        Truncated,
        Reset,
    };

    Kind kind = Invalid;
    int index = -1;
    int length = -1;
    QList<TimelineEvent> items;

    bool liveRemoteInsert = false;
    bool becameDecrypted = false;
    bool becameRedacted = false;
};

TimelineEvent::Type rowTypeForMsgtype(const QString &msgtype);

void readPushVerdict(const QJsonObject &payload, TimelineEvent &event);

TimelineEvent syncEventFromPayload(const QJsonObject &obj, const QString &roomId);

enum class SyncRoute {
    MirrorAndAppend,
    // A room with no open timeline, but a row the mirror must not hold: a
    // thread reply (CLAUDE.md section 8: never a main-timeline row), an edit
    // (its "* " fallback), or a poll start (no answers until the SDK timeline
    // aggregates it). Still announced through eventAppended, de-duplicated by
    // id because the mirror cannot see it again.
    AppendUnmirrored,
    // The open room's thread reply. The live timeline hides threaded events,
    // so no diff will ever announce it; it goes out on liveEventObserved for
    // notifications and the Activity Center only, never to TimelineModel and
    // never into the mirror.
    ObserveOnly,
    PreviewOnly,
};
SyncRoute syncRouteFor(bool roomHasLiveTimeline, const TimelineEvent &event);

TimelineEvent eventFromItemJson(const QJsonObject &item, const QString &roomId);

QList<TimelineEvent> eventsFromItemArray(const QJsonArray &items,
                                         const QString &roomId);

DiffOutcome applyTimelineDiff(QList<TimelineEvent> &mirror,
                              const QJsonObject &diff,
                              const QString &roomId);

QHash<QString, MemberInfo> membersFromPayload(const QJsonArray &rows);

// Tracks which (room, room_generation) the C++ side accepts. Rust mints the
// generation; C++ adopts it from the timeline_reset of the most recently
// requested room and rejects everything else, so stale diffs (previous room,
// previous open, signed-out lifecycle) never mutate visible state.
class TimelineGenerationTracker
{
public:
    void request(const QString &roomId)
    {
        m_requestedRoom = roomId;
        m_activeRoom.clear();
        m_generation = 0;
    }

    bool adoptReset(const QString &roomId, quint64 generation)
    {
        if (roomId.isEmpty() || roomId != m_requestedRoom || generation == 0)
            return false;
        if (roomId == m_activeRoom && generation <= m_generation)
            return false;
        m_activeRoom = roomId;
        m_generation = generation;
        return true;
    }

    bool accepts(const QString &roomId, quint64 generation) const
    {
        return !m_activeRoom.isEmpty() && roomId == m_activeRoom
            && generation == m_generation;
    }

    void reset()
    {
        m_requestedRoom.clear();
        m_activeRoom.clear();
        m_generation = 0;
    }

    QString activeRoom() const { return m_activeRoom; }
    QString requestedRoom() const { return m_requestedRoom; }
    quint64 generation() const { return m_generation; }
    bool hasActiveTimeline() const { return !m_activeRoom.isEmpty(); }
    bool readyForPagination(const QString &roomId) const
    {
        return !roomId.isEmpty() && roomId == m_activeRoom
            && m_generation != 0;
    }

private:
    QString m_requestedRoom;
    QString m_activeRoom;
    quint64 m_generation = 0;
};

}
