// Room-list previews follow the UI language (2026-10-18 live runs, every
// platform: "You joined", "Unable to decrypt" and media rows stayed English
// after switching to German).
//
// A preview that is words rather than the sender's text is kept as a KIND
// (matrix/PreviewToken.h) and worded with the translators installed now:
//   * at ingest, for a kind the Rust bridge sends instead of English words;
//   * again after a switch (RoomInfo::retranslatePreview, which the backends
//     run on AppLocale::changed);
//   * when a cached room is loaded (CacheStore keeps the kind, never the
//     words, and still persists nothing for an encrypted room).

#include "matrix/EventPreview.h"
#include "matrix/RustRoomRegistry.h"
#include "matrix/TimelineEvent.h"
#include "storage/AppDataPaths.h"
#include "storage/CacheStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

#include <optional>

using matrix::preview::Kind;

namespace {

const QString kRoom = QStringLiteral("!lang:example.org");
const QString kUser = QStringLiteral("@lang-test:example.org");

// German for the EventPreview context, nothing else.
struct German : QTranslator {
    QString translate(const char *ctx, const char *src, const char *,
                      int n) const override
    {
        if (QByteArray(ctx) != "EventPreview")
            return {};
        const QByteArray s(src);
        if (s == "You joined") return QStringLiteral("Du bist beigetreten");
        if (s == "Unable to decrypt") return QStringLiteral("Entschlüsselung fehlgeschlagen");
        if (s == "Message removed") return QStringLiteral("Nachricht entfernt");
        if (s == "Image") return QStringLiteral("Bild");
        if (s == "Voice message") return QStringLiteral("Sprachnachricht");
        if (s == "Poll: %1") return QStringLiteral("Umfrage: %1");
        if (s == "%n image(s)")
            return n == 1 ? QStringLiteral("%n Bild") : QStringLiteral("%n Bilder");
        return {};
    }
    bool isEmpty() const override { return false; }
};

// Installs for the scope of one case, so a failing case cannot leave German
// behind for the next one.
struct Installed {
    explicit Installed(QTranslator *t) : t(t) { QCoreApplication::installTranslator(t); }
    ~Installed() { QCoreApplication::removeTranslator(t); }
    QTranslator *t;
};

QJsonObject payload(QJsonObject fields)
{
    fields.insert(QStringLiteral("id"), kRoom);
    return fields;
}

} // namespace

class RoomPreviewLanguageTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dataDir.isValid());
        // Isolate the XDG data root so the cache cases never touch a real one.
        qputenv("XDG_DATA_HOME", m_dataDir.path().toUtf8());
    }

    // What the room list showed in German for a room just joined: "You
    // joined", worded once at ingest and kept. Old code: the text stays
    // English after the switch (and there was nothing to re-word it with).
    void aJoinedRoomIsWordedAgainAfterASwitch()
    {
        RoomInfo room = matrix::rust_rooms::roomInfoFromJson(
            payload({ { QStringLiteral("last_message_own_join"), true } }), RoomInfo{});
        QCOMPARE(room.lastMessagePreview, QStringLiteral("You joined"));
        German german;
        {
            Installed on(&german);
            QVERIFY(room.retranslatePreview());
            QCOMPARE(room.lastMessagePreview, QStringLiteral("Du bist beigetreten"));
            // Nothing more to do in the same language.
            QVERIFY(!room.retranslatePreview());
        }
        QVERIFY(room.retranslatePreview());
        QCOMPARE(room.lastMessagePreview, QStringLiteral("You joined"));
    }

    // The undecryptable placeholder from a room_activity payload, likewise.
    void theUndecryptablePlaceholderIsWordedAgainAfterASwitch()
    {
        QHash<QString, RoomInfo> rooms;
        QStringList order;
        RoomInfo known;
        known.id = kRoom;
        known.setPreviewText(QStringLiteral("older words"));
        rooms.insert(kRoom, known);
        const QStringList moved = matrix::rust_rooms::applyRoomActivity(
            { rooms, order },
            QJsonArray{ payload({ { QStringLiteral("last_activity_ms"), 1700000000000.0 },
                                  { QStringLiteral("last_message_undecryptable"), true } }) });
        QCOMPARE(moved, QStringList{ kRoom });
        QCOMPARE(rooms[kRoom].lastMessagePreview, QStringLiteral("Unable to decrypt"));
        QCOMPARE(rooms[kRoom].previewKind, Kind::Undecryptable);
        German german;
        Installed on(&german);
        QVERIFY(rooms[kRoom].retranslatePreview());
        QCOMPARE(rooms[kRoom].lastMessagePreview,
                 QStringLiteral("Entschlüsselung fehlgeschlagen"));
    }

    // Rust words media rows in English ("Image", "Poll: …", "3 images"); it
    // sends the kind too, and the words are made here. Old code: the English
    // text was shown whatever the language.
    void aKindFromTheBridgeIsWordedInTheUiLanguage()
    {
        German german;
        Installed on(&german);
        const auto ingest = [](const QString &english, const QString &kind,
                               const QString &arg) {
            return matrix::rust_rooms::roomInfoFromJson(
                payload({ { QStringLiteral("last_message_preview"), english },
                          { QStringLiteral("last_message_preview_kind"), kind },
                          { QStringLiteral("last_message_preview_arg"), arg } }),
                RoomInfo{}).lastMessagePreview;
        };
        QCOMPARE(ingest(QStringLiteral("Image"), QStringLiteral("image"), {}),
                 QStringLiteral("Bild"));
        QCOMPARE(ingest(QStringLiteral("Voice message"), QStringLiteral("voice"), {}),
                 QStringLiteral("Sprachnachricht"));
        QCOMPARE(ingest(QStringLiteral("Poll: Lunch?"), QStringLiteral("poll_question"),
                        QStringLiteral("Lunch?")),
                 QStringLiteral("Umfrage: Lunch?"));
        QCOMPARE(ingest(QStringLiteral("3 images"), QStringLiteral("images"),
                        QStringLiteral("3")),
                 QStringLiteral("3 Bilder"));
        // A kind this build does not know: the English words, not a guess.
        QCOMPARE(ingest(QStringLiteral("Hologram"), QStringLiteral("hologram"), {}),
                 QStringLiteral("Hologram"));
        // The sender's own text is text, even when it reads like a label.
        const RoomInfo body = matrix::rust_rooms::roomInfoFromJson(
            payload({ { QStringLiteral("last_message_preview"), QStringLiteral("Image") } }),
            RoomInfo{});
        QCOMPARE(body.lastMessagePreview, QStringLiteral("Image"));
        QCOMPARE(body.previewKind, Kind::Text);
    }

    // A later free-text preview must clear the kind, or the next switch
    // would put the old label back over the new message.
    void aNewMessageClearsTheKind()
    {
        RoomInfo room;
        room.setPreview(Kind::OwnJoin);
        room = matrix::rust_rooms::roomInfoFromJson(
            payload({ { QStringLiteral("last_message_preview"), QStringLiteral("hallo") } }),
            room);
        QCOMPARE(room.previewKind, Kind::Text);
        German german;
        Installed on(&german);
        QVERIFY(!room.retranslatePreview());
        QCOMPARE(room.lastMessagePreview, QStringLiteral("hallo"));
    }

    // The token the live-event path stores words exactly as oneLineSummary.
    void theSummaryTokenSaysWhatTheSummarySays()
    {
        QList<TimelineEvent> events;
        TimelineEvent e;
        e.body = QStringLiteral("plain words");
        events << e;
        e = {};
        e.redacted = true;
        events << e;
        e = {};
        e.undecryptable = true;
        events << e;
        e = {};
        e.type = TimelineEvent::Image;
        events << e;
        e.mediaFilename = QStringLiteral("cat.png");
        events << e;
        e.mediaMimetype = QStringLiteral("image/gif");
        events << e;
        e = {};
        e.type = TimelineEvent::File;
        events << e;
        e.mediaFilename = QStringLiteral("report.pdf");
        events << e;
        e = {};
        e.type = TimelineEvent::Audio;
        e.mediaIsVoice = true;
        events << e;
        e = {};
        e.type = TimelineEvent::Poll;
        events << e;
        e.pollQuestion = QStringLiteral("Lunch?");
        events << e;
        e = {};
        e.type = TimelineEvent::Location;
        e.locationLive = true;
        events << e;
        e = {};
        e.type = TimelineEvent::Image;
        GalleryItem item;
        item.kind = QStringLiteral("image");
        e.galleryItems = { item, item, item };
        events << e;
        German german;
        for (bool translated : { false, true }) {
            std::optional<Installed> on;
            if (translated)
                on.emplace(&german);
            for (const TimelineEvent &event : std::as_const(events)) {
                const auto token = matrix::preview::summaryToken(event);
                QCOMPARE(matrix::preview::render(token.kind, token.arg),
                         matrix::preview::oneLineSummary(event));
            }
        }
    }

    // ---- CacheStore (the HTTP backend's persisted rooms) ----

    // A cache written before the migration keeps every room and its words.
    void aCacheFromBeforeTheMigrationLosesNothing()
    {
        const QString root = matrix::app_data::accountRoot(kUser);
        QVERIFY(QDir().mkpath(root));
        const QString path = root + QStringLiteral("/cache.sqlite");
        QFile::remove(path);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("legacy"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            // The rooms table as 0.10.x created it.
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE rooms (id TEXT PRIMARY KEY, name TEXT, topic TEXT,"
                " avatar_mxc TEXT, encrypted INTEGER, unread INTEGER,"
                " last_activity_ms INTEGER, last_preview TEXT, prev_batch TEXT,"
                " is_space INTEGER NOT NULL DEFAULT 0,"
                " child_room_ids TEXT NOT NULL DEFAULT '')")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO rooms (id, name, encrypted, unread, last_activity_ms,"
                " last_preview, prev_batch) VALUES ('%1', 'Legacy', 0, 2,"
                " 1700000000000, 'see you tomorrow', 'tok')").arg(kRoom)));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy"));

        CacheStore store;
        QVERIFY(store.openFor(kUser));
        const QList<RoomInfo> rooms = store.loadRooms();
        QCOMPARE(rooms.size(), 1);
        QCOMPARE(rooms.first().name, QStringLiteral("Legacy"));
        QCOMPARE(rooms.first().lastMessagePreview, QStringLiteral("see you tomorrow"));
        QCOMPARE(rooms.first().previewKind, Kind::Text);
        QCOMPARE(rooms.first().unreadCount, 2);
        QCOMPARE(rooms.first().prevBatchToken, QStringLiteral("tok"));
        // And the migrated table takes a kind from now on.
        RoomInfo again = rooms.first();
        again.setPreview(Kind::Removed);
        store.saveRoom(again);
        QCOMPARE(store.loadRooms().first().previewKind, Kind::Removed);
        store.clearAll();
        store.close();
    }

    // Review round 1 (L6): a migration that cannot run (here: a read-only
    // cache file, so the ALTER fails) must not cost the cached rooms. The
    // store falls back to the old column list. Old code selected the new
    // columns regardless and loaded nothing.
    void aCacheWhoseMigrationFailedStillLoads()
    {
        const QString root = matrix::app_data::accountRoot(kUser);
        QVERIFY(QDir().mkpath(root));
        const QString path = root + QStringLiteral("/cache.sqlite");
        QFile::remove(path);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("legacy-ro"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            // The whole 0.10.x schema, so only the ALTERs need to write.
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE rooms (id TEXT PRIMARY KEY, name TEXT, topic TEXT,"
                " avatar_mxc TEXT, encrypted INTEGER, unread INTEGER,"
                " last_activity_ms INTEGER, last_preview TEXT, prev_batch TEXT,"
                " is_space INTEGER NOT NULL DEFAULT 0,"
                " child_room_ids TEXT NOT NULL DEFAULT '')")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE events (event_id TEXT PRIMARY KEY, room_id TEXT NOT NULL,"
                " sender TEXT, ts_ms INTEGER, type INTEGER, status INTEGER, body TEXT,"
                " edited INTEGER, redacted INTEGER, reply_to TEXT, reply_sender TEXT,"
                " reply_preview TEXT, media_mxc TEXT, media_mimetype TEXT,"
                " media_filename TEXT, media_size INTEGER, media_width INTEGER,"
                " media_height INTEGER, media_thumb TEXT, reactions TEXT,"
                " thread_root_id TEXT NOT NULL DEFAULT '')")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE INDEX idx_events_room_ts ON events(room_id, ts_ms)")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE members (room_id TEXT NOT NULL, user_id TEXT NOT NULL,"
                " display_name TEXT, avatar_mxc TEXT, PRIMARY KEY(room_id, user_id))")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO rooms (id, name, encrypted, last_preview) "
                "VALUES ('%1', 'Kept', 0, 'still here')").arg(kRoom)));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy-ro"));
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadUser));
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
            QFile::remove(path);
        });

        CacheStore store;
        QVERIFY(store.openFor(kUser));
        const QList<RoomInfo> rooms = store.loadRooms();
        QCOMPARE(rooms.size(), 1);
        QCOMPARE(rooms.first().lastMessagePreview, QStringLiteral("still here"));
        QCOMPARE(rooms.first().previewKind, Kind::Text);
        store.close();
    }

    // The words of a cached kind are made when the cache is read, in the
    // language current then. Old code: the cache held the words.
    void aCachedKindIsWordedInTheLanguageOfTheLoad()
    {
        {
            CacheStore store;
            QVERIFY(store.openFor(kUser));
            store.clearAll();
            RoomInfo room;
            room.id = kRoom;
            room.name = QStringLiteral("Plain");
            room.setPreview(Kind::Removed);
            store.saveRoom(room);
            store.close();
        }
        German german;
        Installed on(&german);
        CacheStore store;
        QVERIFY(store.openFor(kUser));
        const QList<RoomInfo> rooms = store.loadRooms();
        QCOMPARE(rooms.size(), 1);
        QCOMPARE(rooms.first().lastMessagePreview, QStringLiteral("Nachricht entfernt"));
        QCOMPARE(rooms.first().previewKind, Kind::Removed);
        store.clearAll();
        store.close();
    }

    // CLAUDE.md §6: an encrypted room's preview never reaches the cache. The
    // kind's argument can be decrypted text (a file name, a poll question)
    // and the kind itself says what the message was, so neither is written.
    void anEncryptedRoomPersistsNoKindAndNoArgument()
    {
        const QString marker = QStringLiteral("LIGHTNING_PREVIEW_ARG_MARKER_R9");
        {
            CacheStore store;
            QVERIFY(store.openFor(kUser));
            store.clearAll();
            RoomInfo room;
            room.id = kRoom;
            room.name = QStringLiteral("Secret");
            room.encrypted = true;
            room.setPreview(Kind::FileNamed, marker);
            QVERIFY(room.lastMessagePreview.contains(marker));
            store.saveRoom(room);
            const QList<RoomInfo> rooms = store.loadRooms();
            QCOMPARE(rooms.size(), 1);
            QCOMPARE(rooms.first().previewKind, Kind::Text);
            QVERIFY(rooms.first().previewArg.isEmpty());
            QVERIFY(rooms.first().lastMessagePreview.isEmpty());
            store.close();
        }
        QDirIterator it(m_dataDir.path(), { QStringLiteral("cache.sqlite*") },
                        QDir::Files, QDirIterator::Subdirectories);
        int files = 0;
        while (it.hasNext()) {
            QFile file(it.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY2(!file.readAll().contains(marker.toUtf8()),
                     "an encrypted room's preview argument reached the cache file");
            ++files;
        }
        QVERIFY(files >= 1);
        CacheStore cleanup;
        QVERIFY(cleanup.openFor(kUser));
        cleanup.clearAll();
    }

private:
    QTemporaryDir m_dataDir;
};

QTEST_GUILESS_MAIN(RoomPreviewLanguageTest)
#include "RoomPreviewLanguageTest.moc"
