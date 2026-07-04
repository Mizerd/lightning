#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

// A minimal per-user SQLite cache for rooms, timeline events and per-room
// members. Backed by QSqlDatabase (SQLite driver) under
// ${XDG_DATA_HOME}/matrix-client/<safeUserId>/cache.sqlite.
//
// v0.3 scope:
//   - Store enough state to render the room list + last N timeline events
//     for each room instantly on startup, before /whoami or /sync run.
//   - Persist member display names / avatar mxc URLs so senders don't render
//     as raw MXIDs after a restart.
//
// Explicitly NOT stored here (see docs/threat-model.md):
//   - access_token or any other bearer credential — those still live in
//     SettingsManager (plaintext QSettings until v0.4 moves them behind a
//     keychain abstraction).
//   - Encrypted-message plaintext (no crypto backend yet).
//   - Full timeline history — we cap per-room at kEventCap most recent events
//     to avoid unbounded growth.
class CacheStore : public QObject
{
    Q_OBJECT
public:
    static constexpr int kEventCap = 200;

    explicit CacheStore(QObject *parent = nullptr);
    ~CacheStore() override;

    // Opens (creating if needed) the cache for a given MXID. Returns false
    // on I/O or SQL failure; callers should proceed as if there is no cache.
    bool openFor(const QString &userId);
    void close();
    bool isOpen() const;

    // Wipe everything on this session (logout).
    void clearAll();

    // Room summaries.
    QList<RoomInfo> loadRooms();
    void saveRoom(const RoomInfo &room);
    void deleteRoom(const QString &roomId);

    // Timeline: bounded per room, most recent kEventCap entries.
    QList<TimelineEvent> loadTimeline(const QString &roomId,
                                      int limit = kEventCap);
    void appendEvent(const TimelineEvent &e);
    void updateEvent(const TimelineEvent &e);          // full row upsert by eventId
    void markEventRedacted(const QString &eventId);
    void deleteEvent(const QString &eventId);
    void replaceEventId(const QString &oldEventId,
                        const TimelineEvent &newEvent);

    // Members.
    QHash<QString, MemberInfo> loadMembers(const QString &roomId);
    void saveMember(const QString &roomId, const MemberInfo &member);

private:
    QString m_userId;
    QString m_dbPath;
    QString m_connectionName;
    QSqlDatabase database() const;
    bool ensureSchema();

    // Prunes rows so no room keeps more than kEventCap timeline events.
    void pruneRoom(const QString &roomId);
};
