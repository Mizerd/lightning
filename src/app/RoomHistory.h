#pragma once

#include <QString>
#include <QStringList>
#include <QList>

#include <functional>

namespace lightning {

// Back/forward history of opened rooms, like a browser's tab history: pure
// navigation state, in memory only. Nothing here is ever written to settings
// or disk, and AppController clears it on sign-out and account switch.
//
// The list holds at most kMaxBackEntries rooms behind the open one. `visit()`
// records an opening (an empty id means "no room open": the lobby, Space
// Home, Settings-less views), `back()` / `forward()` move the cursor and hand
// back the room to open. A new visit after going back drops the forward list.
// Rooms the caller reports as unavailable (left, forgotten) are removed
// rather than offered.
class RoomHistory
{
public:
    static constexpr int kMaxBackEntries = 10;
    struct Place {
        enum Kind { Room, Thread, SearchResult } kind = Room;
        QString roomId;
        QString rootEventId;
        QString eventId;
        bool operator==(const Place &) const = default;
    };

    // True for a room that can still be opened (joined or invited, not
    // left or forgotten).
    using Openable = std::function<bool(const QString &roomId)>;

    // Records the user opening `roomId`, or closing the open room (empty).
    void visit(const QString &roomId);
    void visitThread(const QString &roomId, const QString &rootEventId);
    void visitSearchResult(const QString &roomId, const QString &eventId);

    // Moves one step and returns the room to open, or an empty string when
    // there is nowhere to go (the state is then unchanged apart from pruning
    // unavailable rooms).
    QString back(const Openable &openable);
    QString forward(const Openable &openable);
    Place backPlace(const Openable &openable);
    Place forwardPlace(const Openable &openable);

    // Non-mutating: would back() / forward() find a room?
    bool canGoBack(const Openable &openable) const;
    bool canGoForward(const Openable &openable) const;

    void clear();

    // For tests and diagnostics (room ids are not logged by callers).
    QStringList entries() const;
    QList<Place> places() const { return m_entries; }
    int cursor() const { return m_cursor; }
    bool detached() const { return m_detached; }

private:
    void prune(const Openable &openable);
    void trim();
    void visitPlace(const Place &place);
    int backTarget() const { return m_detached ? m_cursor : m_cursor - 1; }
    int forwardTarget() const { return m_cursor + 1; }

    QList<Place> m_entries;
    // The entry the open room is, or, while `m_detached`, the last room that
    // was open. -1 only while detached with nothing behind it.
    int m_cursor = -1;
    // No room is open (the lobby or a Space overview); Back returns to the
    // entry at the cursor, Forward to the one after it.
    bool m_detached = false;
};

} // namespace lightning
