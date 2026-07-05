#include "storage/CacheStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

Q_LOGGING_CATEGORY(lcCache, "matrix.cache")

namespace {

QString safeSlug(const QString &userId)
{
    // "@alice:example.com" → "alice_example.com"
    QString s = userId;
    if (s.startsWith(QLatin1Char('@')))
        s.remove(0, 1);
    s.replace(QLatin1Char(':'), QLatin1Char('_'));
    s.replace(QLatin1Char('/'), QLatin1Char('_'));
    return s;
}

QByteArray encodeReactions(const QList<Reaction> &reactions)
{
    QJsonArray arr;
    for (const auto &r : reactions) {
        arr.push_back(QJsonObject{
            { QStringLiteral("key"),   r.key },
            { QStringLiteral("count"), r.count },
            { QStringLiteral("byMe"),  r.byMe },
            { QStringLiteral("myId"),  r.myEventId },
        });
    }
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QList<Reaction> decodeReactions(const QByteArray &blob)
{
    QList<Reaction> out;
    if (blob.isEmpty()) return out;
    const auto doc = QJsonDocument::fromJson(blob);
    if (!doc.isArray()) return out;
    for (const auto &v : doc.array()) {
        const auto o = v.toObject();
        Reaction r;
        r.key       = o.value(QStringLiteral("key")).toString();
        r.count     = o.value(QStringLiteral("count")).toInt();
        r.byMe      = o.value(QStringLiteral("byMe")).toBool();
        r.myEventId = o.value(QStringLiteral("myId")).toString();
        out.append(r);
    }
    return out;
}

} // namespace

CacheStore::CacheStore(QObject *parent)
    : QObject(parent)
{}

CacheStore::~CacheStore()
{
    close();
}

QSqlDatabase CacheStore::database() const
{
    return QSqlDatabase::database(m_connectionName, /*open=*/false);
}

bool CacheStore::openFor(const QString &userId)
{
    close();

    const QString root = QStandardPaths::writableLocation(
                             QStandardPaths::AppLocalDataLocation)
                         + QLatin1Char('/')
                         + safeSlug(userId);
    if (!QDir().mkpath(root)) {
        qCWarning(lcCache) << "cannot create cache dir" << root;
        return false;
    }
    m_userId = userId;
    m_dbPath = root + QStringLiteral("/cache.sqlite");
    m_connectionName = QStringLiteral("matrix-cache-")
                       + QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                m_connectionName);
    db.setDatabaseName(m_dbPath);
    if (!db.open()) {
        qCWarning(lcCache) << "open failed" << db.lastError().text();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return false;
    }
    if (!ensureSchema()) {
        db.close();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
        return false;
    }
    qCInfo(lcCache) << "cache open" << m_dbPath;
    return true;
}

void CacheStore::close()
{
    if (m_connectionName.isEmpty())
        return;
    {
        QSqlDatabase db = database();
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
    m_connectionName.clear();
    m_dbPath.clear();
    m_userId.clear();
}

bool CacheStore::isOpen() const
{
    if (m_connectionName.isEmpty())
        return false;
    return database().isOpen();
}

// True if the given column already exists on the given SQLite table.
static bool columnExists(QSqlDatabase &db,
                         const QString &table,
                         const QString &column)
{
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (q.next()) {
        if (q.value(1).toString() == column)
            return true;
    }
    return false;
}

bool CacheStore::ensureSchema()
{
    QSqlDatabase db = database();
    QSqlQuery q(db);

    static const char *ddl[] = {
        // v0.4.5 note: for fresh databases we declare the full column set
        // up front. For pre-v0.4.5 databases the ALTER TABLE migration
        // below adds the same columns idempotently — either path arrives
        // at the same schema.
        "CREATE TABLE IF NOT EXISTS rooms ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT,"
        "  topic TEXT,"
        "  avatar_mxc TEXT,"
        "  encrypted INTEGER,"
        "  unread INTEGER,"
        "  last_activity_ms INTEGER,"
        "  last_preview TEXT,"
        "  prev_batch TEXT,"
        "  is_space INTEGER NOT NULL DEFAULT 0,"
        "  child_room_ids TEXT NOT NULL DEFAULT ''"
        ")",
        "CREATE TABLE IF NOT EXISTS events ("
        "  event_id TEXT PRIMARY KEY,"
        "  room_id TEXT NOT NULL,"
        "  sender TEXT,"
        "  ts_ms INTEGER,"
        "  type INTEGER,"
        "  status INTEGER,"
        "  body TEXT,"
        "  edited INTEGER,"
        "  redacted INTEGER,"
        "  reply_to TEXT,"
        "  reply_sender TEXT,"
        "  reply_preview TEXT,"
        "  media_mxc TEXT,"
        "  media_mimetype TEXT,"
        "  media_filename TEXT,"
        "  media_size INTEGER,"
        "  media_width INTEGER,"
        "  media_height INTEGER,"
        "  media_thumb TEXT,"
        "  reactions TEXT,"
        "  thread_root_id TEXT NOT NULL DEFAULT ''"
        ")",
        "CREATE INDEX IF NOT EXISTS idx_events_room_ts "
        "  ON events(room_id, ts_ms)",
        "CREATE TABLE IF NOT EXISTS members ("
        "  room_id TEXT NOT NULL,"
        "  user_id TEXT NOT NULL,"
        "  display_name TEXT,"
        "  avatar_mxc TEXT,"
        "  PRIMARY KEY(room_id, user_id)"
        ")",
    };
    for (const char *sql : ddl) {
        if (!q.exec(QLatin1String(sql))) {
            qCWarning(lcCache) << "schema failed" << q.lastError().text();
            return false;
        }
    }

    // v0.4.5 migrations: bring existing databases up to the current schema
    // without destroying user data. SQLite's ALTER TABLE ADD COLUMN is
    // additive-only and safe; we probe with PRAGMA table_info first so we
    // don't spam warnings on a fresh database that already has the column.
    struct AddCol { const char *table; const char *col; const char *ddl; };
    static const AddCol adds[] = {
        { "rooms",  "is_space",
          "ALTER TABLE rooms ADD COLUMN is_space INTEGER NOT NULL DEFAULT 0" },
        { "rooms",  "child_room_ids",
          "ALTER TABLE rooms ADD COLUMN child_room_ids TEXT NOT NULL DEFAULT ''" },
        { "events", "thread_root_id",
          "ALTER TABLE events ADD COLUMN thread_root_id TEXT NOT NULL DEFAULT ''" },
    };
    for (const auto &a : adds) {
        if (columnExists(db, QLatin1String(a.table), QLatin1String(a.col)))
            continue;
        QSqlQuery m(db);
        if (!m.exec(QLatin1String(a.ddl))) {
            qCWarning(lcCache) << "migration failed for" << a.table << a.col
                               << m.lastError().text();
            // Non-fatal: caller can still operate; new fields just won't
            // persist for that user.
        } else {
            qCInfo(lcCache) << "migrated" << a.table << "->" << a.col;
        }
    }
    return true;
}

void CacheStore::clearAll()
{
    if (!isOpen()) return;
    QSqlQuery q(database());
    q.exec(QStringLiteral("DELETE FROM events"));
    q.exec(QStringLiteral("DELETE FROM rooms"));
    q.exec(QStringLiteral("DELETE FROM members"));
}

QList<RoomInfo> CacheStore::loadRooms()
{
    QList<RoomInfo> out;
    if (!isOpen()) return out;
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "SELECT id, name, topic, avatar_mxc, encrypted, unread, "
        "       last_activity_ms, last_preview, prev_batch, "
        "       is_space, child_room_ids "
        "FROM rooms"));
    if (!q.exec()) {
        qCWarning(lcCache) << "loadRooms" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        RoomInfo r;
        r.id                 = q.value(0).toString();
        r.name               = q.value(1).toString();
        r.topic              = q.value(2).toString();
        r.avatarUrl          = q.value(3).toString();
        r.encrypted          = q.value(4).toBool();
        r.unreadCount        = q.value(5).toInt();
        const qint64 ms      = q.value(6).toLongLong();
        if (ms > 0)
            r.lastActivity   = QDateTime::fromMSecsSinceEpoch(ms);
        r.lastMessagePreview = q.value(7).toString();
        r.prevBatchToken     = q.value(8).toString();
        r.isSpace            = q.value(9).toBool();
        const QString ids    = q.value(10).toString();
        if (!ids.isEmpty()) {
            // Matrix room ids never contain commas, so comma-joining is safe.
            r.childRoomIds = ids.split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
        // members loaded separately via loadMembers()
        out.append(r);
    }
    return out;
}

void CacheStore::saveRoom(const RoomInfo &r)
{
    if (!isOpen()) return;
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "INSERT INTO rooms (id, name, topic, avatar_mxc, encrypted, unread, "
        "                   last_activity_ms, last_preview, prev_batch, "
        "                   is_space, child_room_ids) "
        "VALUES (:id, :name, :topic, :avatar, :enc, :unread, :ts, :prev, :prevBatch,"
        "        :space, :children) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  name=excluded.name, topic=excluded.topic, avatar_mxc=excluded.avatar_mxc,"
        "  encrypted=excluded.encrypted, unread=excluded.unread,"
        "  last_activity_ms=excluded.last_activity_ms,"
        "  last_preview=excluded.last_preview,"
        "  prev_batch=excluded.prev_batch,"
        "  is_space=excluded.is_space,"
        "  child_room_ids=excluded.child_room_ids"));
    q.bindValue(QStringLiteral(":id"),        r.id);
    q.bindValue(QStringLiteral(":name"),      r.name);
    q.bindValue(QStringLiteral(":topic"),     r.topic);
    q.bindValue(QStringLiteral(":avatar"),    r.avatarUrl);
    q.bindValue(QStringLiteral(":enc"),       r.encrypted ? 1 : 0);
    q.bindValue(QStringLiteral(":unread"),    r.unreadCount);
    q.bindValue(QStringLiteral(":ts"),
                r.lastActivity.isValid() ? r.lastActivity.toMSecsSinceEpoch() : 0);
    q.bindValue(QStringLiteral(":prev"),      r.lastMessagePreview);
    q.bindValue(QStringLiteral(":prevBatch"), r.prevBatchToken);
    q.bindValue(QStringLiteral(":space"),     r.isSpace ? 1 : 0);
    q.bindValue(QStringLiteral(":children"),  r.childRoomIds.join(QLatin1Char(',')));
    if (!q.exec())
        qCWarning(lcCache) << "saveRoom" << q.lastError().text();
}

void CacheStore::deleteRoom(const QString &roomId)
{
    if (!isOpen()) return;
    QSqlQuery q(database());
    q.prepare(QStringLiteral("DELETE FROM events WHERE room_id = :r"));
    q.bindValue(QStringLiteral(":r"), roomId);
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM members WHERE room_id = :r"));
    q.bindValue(QStringLiteral(":r"), roomId);
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM rooms WHERE id = :r"));
    q.bindValue(QStringLiteral(":r"), roomId);
    q.exec();
}

static TimelineEvent readEvent(QSqlQuery &q)
{
    TimelineEvent e;
    e.eventId           = q.value(0).toString();
    e.roomId            = q.value(1).toString();
    e.sender            = q.value(2).toString();
    const qint64 ts     = q.value(3).toLongLong();
    if (ts > 0) e.timestamp = QDateTime::fromMSecsSinceEpoch(ts);
    e.type              = static_cast<TimelineEvent::Type>(q.value(4).toInt());
    e.status            = static_cast<TimelineEvent::Status>(q.value(5).toInt());
    e.body              = q.value(6).toString();
    e.edited            = q.value(7).toBool();
    e.redacted          = q.value(8).toBool();
    e.replyToEventId    = q.value(9).toString();
    e.replyToSender     = q.value(10).toString();
    e.replyToPreview    = q.value(11).toString();
    e.mediaMxcUrl       = q.value(12).toString();
    e.mediaMimetype     = q.value(13).toString();
    e.mediaFilename     = q.value(14).toString();
    e.mediaSize         = q.value(15).toLongLong();
    e.mediaWidth        = q.value(16).toInt();
    e.mediaHeight       = q.value(17).toInt();
    e.mediaThumbnailMxcUrl = q.value(18).toString();
    e.reactions         = decodeReactions(q.value(19).toByteArray());
    e.threadRootId      = q.value(20).toString();
    return e;
}

QList<TimelineEvent> CacheStore::loadTimeline(const QString &roomId, int limit)
{
    QList<TimelineEvent> out;
    if (!isOpen()) return out;

    // Fetch newest N in descending order, then reverse to chronological.
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "SELECT event_id, room_id, sender, ts_ms, type, status, body, "
        "       edited, redacted, reply_to, reply_sender, reply_preview, "
        "       media_mxc, media_mimetype, media_filename, media_size, "
        "       media_width, media_height, media_thumb, reactions, "
        "       thread_root_id "
        "FROM events WHERE room_id = :r "
        "ORDER BY ts_ms DESC, event_id DESC LIMIT :lim"));
    q.bindValue(QStringLiteral(":r"),   roomId);
    q.bindValue(QStringLiteral(":lim"), limit);
    if (!q.exec()) {
        qCWarning(lcCache) << "loadTimeline" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.prepend(readEvent(q));
    return out;
}

void CacheStore::appendEvent(const TimelineEvent &e)
{
    updateEvent(e);
    pruneRoom(e.roomId);
}

void CacheStore::updateEvent(const TimelineEvent &e)
{
    if (!isOpen() || e.eventId.isEmpty()) return;
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "INSERT INTO events "
        "  (event_id, room_id, sender, ts_ms, type, status, body, edited, redacted,"
        "   reply_to, reply_sender, reply_preview,"
        "   media_mxc, media_mimetype, media_filename, media_size,"
        "   media_width, media_height, media_thumb, reactions,"
        "   thread_root_id) "
        "VALUES "
        "  (:id, :room, :sender, :ts, :type, :status, :body, :edited, :redacted,"
        "   :reply, :replyS, :replyP,"
        "   :mxc, :mime, :fname, :size,"
        "   :w, :h, :thumb, :react,"
        "   :thread) "
        "ON CONFLICT(event_id) DO UPDATE SET "
        "   sender=excluded.sender, ts_ms=excluded.ts_ms, type=excluded.type,"
        "   status=excluded.status, body=excluded.body,"
        "   edited=excluded.edited, redacted=excluded.redacted,"
        "   reply_to=excluded.reply_to, reply_sender=excluded.reply_sender,"
        "   reply_preview=excluded.reply_preview,"
        "   media_mxc=excluded.media_mxc, media_mimetype=excluded.media_mimetype,"
        "   media_filename=excluded.media_filename, media_size=excluded.media_size,"
        "   media_width=excluded.media_width, media_height=excluded.media_height,"
        "   media_thumb=excluded.media_thumb, reactions=excluded.reactions,"
        "   thread_root_id=excluded.thread_root_id"));
    q.bindValue(QStringLiteral(":id"),      e.eventId);
    q.bindValue(QStringLiteral(":room"),    e.roomId);
    q.bindValue(QStringLiteral(":sender"),  e.sender);
    q.bindValue(QStringLiteral(":ts"),
                e.timestamp.isValid() ? e.timestamp.toMSecsSinceEpoch() : 0);
    q.bindValue(QStringLiteral(":type"),    static_cast<int>(e.type));
    q.bindValue(QStringLiteral(":status"),  static_cast<int>(e.status));
    q.bindValue(QStringLiteral(":body"),    e.body);
    q.bindValue(QStringLiteral(":edited"),  e.edited ? 1 : 0);
    q.bindValue(QStringLiteral(":redacted"),e.redacted ? 1 : 0);
    q.bindValue(QStringLiteral(":reply"),   e.replyToEventId);
    q.bindValue(QStringLiteral(":replyS"),  e.replyToSender);
    q.bindValue(QStringLiteral(":replyP"),  e.replyToPreview);
    q.bindValue(QStringLiteral(":mxc"),     e.mediaMxcUrl);
    q.bindValue(QStringLiteral(":mime"),    e.mediaMimetype);
    q.bindValue(QStringLiteral(":fname"),   e.mediaFilename);
    q.bindValue(QStringLiteral(":size"),    e.mediaSize);
    q.bindValue(QStringLiteral(":w"),       e.mediaWidth);
    q.bindValue(QStringLiteral(":h"),       e.mediaHeight);
    q.bindValue(QStringLiteral(":thumb"),   e.mediaThumbnailMxcUrl);
    q.bindValue(QStringLiteral(":react"),   encodeReactions(e.reactions));
    q.bindValue(QStringLiteral(":thread"),  e.threadRootId);
    if (!q.exec())
        qCWarning(lcCache) << "updateEvent" << q.lastError().text();
}

void CacheStore::markEventRedacted(const QString &eventId)
{
    if (!isOpen() || eventId.isEmpty()) return;
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "UPDATE events SET redacted=1, body='' WHERE event_id=:id"));
    q.bindValue(QStringLiteral(":id"), eventId);
    q.exec();
}

void CacheStore::deleteEvent(const QString &eventId)
{
    if (!isOpen() || eventId.isEmpty()) return;
    QSqlQuery q(database());
    q.prepare(QStringLiteral("DELETE FROM events WHERE event_id=:id"));
    q.bindValue(QStringLiteral(":id"), eventId);
    q.exec();
}

void CacheStore::replaceEventId(const QString &oldEventId,
                                const TimelineEvent &newEvent)
{
    if (!isOpen()) return;
    // If a real event with the new id already exists (e.g. from /sync), just
    // remove the local placeholder.
    QSqlQuery lookup(database());
    lookup.prepare(QStringLiteral(
        "SELECT event_id FROM events WHERE event_id=:id"));
    lookup.bindValue(QStringLiteral(":id"), newEvent.eventId);
    if (lookup.exec() && lookup.next()) {
        deleteEvent(oldEventId);
        return;
    }
    deleteEvent(oldEventId);
    updateEvent(newEvent);
}

void CacheStore::pruneRoom(const QString &roomId)
{
    if (!isOpen()) return;
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "DELETE FROM events WHERE event_id IN ("
        "  SELECT event_id FROM events WHERE room_id=:r "
        "  ORDER BY ts_ms DESC, event_id DESC LIMIT -1 OFFSET :cap"
        ")"));
    q.bindValue(QStringLiteral(":r"),   roomId);
    q.bindValue(QStringLiteral(":cap"), kEventCap);
    q.exec();
}

QHash<QString, MemberInfo> CacheStore::loadMembers(const QString &roomId)
{
    QHash<QString, MemberInfo> out;
    if (!isOpen()) return out;
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "SELECT user_id, display_name, avatar_mxc "
        "FROM members WHERE room_id=:r"));
    q.bindValue(QStringLiteral(":r"), roomId);
    if (!q.exec()) return out;
    while (q.next()) {
        MemberInfo m;
        m.userId       = q.value(0).toString();
        m.displayName  = q.value(1).toString();
        m.avatarMxcUrl = q.value(2).toString();
        out.insert(m.userId, m);
    }
    return out;
}

void CacheStore::saveMember(const QString &roomId, const MemberInfo &m)
{
    if (!isOpen()) return;
    QSqlQuery q(database());
    q.prepare(QStringLiteral(
        "INSERT INTO members (room_id, user_id, display_name, avatar_mxc) "
        "VALUES (:r, :u, :n, :a) "
        "ON CONFLICT(room_id, user_id) DO UPDATE SET "
        "  display_name=excluded.display_name, avatar_mxc=excluded.avatar_mxc"));
    q.bindValue(QStringLiteral(":r"), roomId);
    q.bindValue(QStringLiteral(":u"), m.userId);
    q.bindValue(QStringLiteral(":n"), m.displayName);
    q.bindValue(QStringLiteral(":a"), m.avatarMxcUrl);
    q.exec();
}
