#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// Pure translation between the Rust bridge's live-timeline JSON
// (timeline_reset / timeline_diff) and the C++ TimelineEvent mirror. No Qt
// models, FFI or I/O, so fully unit-testable.
//
// matrix-sdk-ui owns item identity and index math. Every index/count is
// validated against the mirror first; an invalid diff leaves it untouched and
// reports DiffOutcome::Invalid so the caller can request a full reset.
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

    // Facts for the notification path, which the model signals above cannot
    // carry. None of them changes what the mirror holds.
    //
    // Inserted: a remote event that landed after the last remote item, with
    // only local echoes (pending, uploading or failed sends) or virtual rows
    // after it. matrix-sdk-ui inserts a new remote event there instead of
    // pushing it back while an echo sits at the bottom, so it is a live
    // arrival exactly like an append. A pagination insert never qualifies: it
    // lands before every remote item already loaded. Also set when the bridge
    // marked the diff `live_append` (rust/src/timeline.rs judges it against
    // its own copy of the vector and the timeline's open time).
    bool liveRemoteInsert = false;
    // Changed: the same event went from undecryptable to readable, or from
    // present to redacted. Matched on event id, so a Set that reuses a slot
    // for another event is neither.
    bool becameDecrypted = false;
    bool becameRedacted = false;
};

// Row kind for one bridge `msgtype` (the Rust vocabulary: "text", "notice",
// "emote", "image", "video", "audio", "file", "sticker", "poll", "location",
// "state", "call", "encrypted", "redacted"), or Unknown. Shared by the
// live-timeline ingest and the sync `timeline_event` path so the two
// producers cannot disagree.
TimelineEvent::Type rowTypeForMsgtype(const QString &msgtype);

// The account's push-rule verdict from an event payload (`push_notify`,
// `push_highlight`, `push_sound`), shared by both producers like
// rowTypeForMsgtype. A payload without `push_notify` leaves the event's
// verdict Unknown and both tweaks false.
void readPushVerdict(const QJsonObject &payload, TimelineEvent &event);

// One `timeline_event` payload from the sync handlers (a room with no open
// timeline, or the open room's copy of an event its live timeline also
// shows) as a TimelineEvent. Pure: the caller fills the sender's display
// name and the undecryptable placeholder, which need the client's member
// cache and its translation context. An unknown msgtype becomes a text row,
// unlike the live ingest, because a side surface has nothing better to show.
TimelineEvent syncEventFromPayload(const QJsonObject &obj, const QString &roomId);

// Where a sync-handler event goes.
enum class SyncRoute {
    // A room with no open timeline: into the background mirror (bounded,
    // de-duplicated) and out through eventAppended.
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
    // The open room's other events: its SDK timeline announces them. Only the
    // room-list preview is updated.
    PreviewOnly,
};
SyncRoute syncRouteFor(bool roomHasLiveTimeline, const TimelineEvent &event);

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

// Translate room_members rows into member-cache entries (the cache behind
// displayNameFor()/avatarMxcFor()). Rows without a user id are dropped; name
// and avatar may be empty.
QHash<QString, MemberInfo> membersFromPayload(const QJsonArray &rows);

// Tracks which (room, room_generation) the C++ side accepts. Rust mints the
// generation; C++ adopts it from the timeline_reset of the most recently
// requested room and rejects everything else, so stale diffs (previous room,
// previous open, signed-out lifecycle) never mutate visible state.
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

    // Adopt a reset snapshot. Returns false when the reset is for a room we did
    // not just request, or is older than what we adopted.
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

} // namespace matrix::rust_timeline
