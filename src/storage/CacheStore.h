#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

// Per-user SQLite cache for rooms, recent timeline events and members,
// under ${XDG_DATA_HOME}/MatrixClient/matrix-client/<safeUserId>/cache.sqlite,
// so the room list and recent events render at startup before sync.
//
// Never stored here (see docs/threat-model.md):
//   - access tokens or other credentials (SecretStore holds those);
//   - encrypted-message plaintext: encrypted rows are skipped;
//   - full history: each room keeps at most kEventCap recent events.
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
