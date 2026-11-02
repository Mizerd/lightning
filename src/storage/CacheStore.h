#pragma once

#include "matrix/RoomInfo.h"
#include "matrix/TimelineEvent.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

class CacheStore : public QObject
{
    Q_OBJECT
public:
    static constexpr int kEventCap = 200;

    explicit CacheStore(QObject *parent = nullptr);
    ~CacheStore() override;

    bool openFor(const QString &userId);
    void close();
    bool isOpen() const;

    void clearAll();

    QList<RoomInfo> loadRooms();
    void saveRoom(const RoomInfo &room);
    void deleteRoom(const QString &roomId);

    QList<TimelineEvent> loadTimeline(const QString &roomId,
                                      int limit = kEventCap);
    void appendEvent(const TimelineEvent &e);
    void updateEvent(const TimelineEvent &e);
    void markEventRedacted(const QString &eventId);
    void deleteEvent(const QString &eventId);
    void replaceEventId(const QString &oldEventId,
                        const TimelineEvent &newEvent);

    QHash<QString, MemberInfo> loadMembers(const QString &roomId);
    void saveMember(const QString &roomId, const MemberInfo &member);

private:
    QString m_userId;
    QString m_dbPath;
    // Whether rooms has preview_kind/preview_arg. A failed migration leaves
    // them out, and loadRooms/saveRoom then use the old column list rather
    // than failing outright and losing every cached room.
    bool m_previewColumns = false;
    QString m_connectionName;
    QSqlDatabase database() const;
    bool ensureSchema();

    void pruneRoom(const QString &roomId);
};
