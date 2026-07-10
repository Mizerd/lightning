#pragma once

#include "matrix/TimelineEvent.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// v0.5.7: pure translation layer between the Rust bridge's live-timeline
// JSON protocol (timeline_reset / timeline_diff envelopes) and the C++
// TimelineEvent mirror. No Qt models, no FFI, no I/O — fully unit-testable
// without cargo or a homeserver.
//
// Identity contract: the Rust side (matrix-sdk-ui) owns item identity and
// index math. This layer validates every index/count against the local
// mirror before mutating it; an invalid diff leaves the mirror untouched
// and reports DiffOutcome::Invalid so the caller can request one full
// timeline reset instead of corrupting model state.
namespace matrix::rust_timeline {

// One applied diff, described so the caller can emit the matching granular
// Qt model signals without re-deriving indices.
struct DiffOutcome {
    enum Kind {
        Invalid,    // malformed op / out-of-range index; mirror untouched
        Appended,   // items appended at the end (append / push_back)
        Prepended,  // items prepended at the front (push_front)
        Inserted,   // single item inserted at `index`
        Changed,    // single item replaced in place at `index`
        Removed,    // single item removed at `index` (remove/pop_front/pop_back)
        Cleared,    // mirror emptied (clear)
        Truncated,  // mirror shortened to `length` (truncate)
        Reset,      // mirror replaced wholesale (reset)
    };

    Kind kind = Invalid;
    int index = -1;              // Inserted / Changed / Removed
    int length = -1;             // Truncated: new length
    QList<TimelineEvent> items;  // Appended / Prepended / Inserted / Changed / Reset
};

// Convert one Rust item payload into a TimelineEvent. Virtual rows
// (date_divider / read_marker / timeline_start) map to the corresponding
// TimelineEvent virtual types. Undecryptable rows get the localized
// placeholder body; ciphertext never appears in the payload by contract.
TimelineEvent eventFromItemJson(const QJsonObject &item, const QString &roomId);

QList<TimelineEvent> eventsFromItemArray(const QJsonArray &items,
                                         const QString &roomId);

// Validate and apply a single timeline_diff envelope to `mirror`.
DiffOutcome applyTimelineDiff(QList<TimelineEvent> &mirror,
                              const QJsonObject &diff,
                              const QString &roomId);

// Tracks which (room, room_generation) pair the C++ side currently accepts.
// The generation is minted by Rust; C++ adopts it from the timeline_reset
// snapshot of the most recently *requested* room and rejects everything
// else — stale diffs from a previous room, a previous open of the same
// room, or a signed-out lifecycle can never mutate visible state.
class TimelineGenerationTracker
{
public:
    // A new open-room request invalidates the active adoption until the
    // matching reset arrives.
    void request(const QString &roomId)
    {
        m_requestedRoom = roomId;
        m_activeRoom.clear();
        m_generation = 0;
    }

    // Adopt a reset snapshot. Returns false (no adoption) when the reset is
    // for a room we did not just request, or is older than what we already
    // adopted for it.
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

    // True when an incremental event for (room, generation) may be applied.
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

private:
    QString m_requestedRoom;
    QString m_activeRoom;
    quint64 m_generation = 0;
};

} // namespace matrix::rust_timeline
