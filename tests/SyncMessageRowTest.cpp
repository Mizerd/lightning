
#include "app/SettingsManager.h"
#include "matrix/EventPreview.h"
#include "matrix/TimelineEvent.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QTimeZone>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

class SyncMessageRowTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static QJsonObject syncedMessage(const QString &msgtype,
                                     const QString &body,
                                     const QString &mediaFilename,
                                     const QString &eventId)
    {
        QJsonObject event;
        event.insert(QStringLiteral("event_id"), eventId);
        event.insert(QStringLiteral("sender"), QStringLiteral("@a:example.org"));
        event.insert(QStringLiteral("body"), body);
        event.insert(QStringLiteral("media_filename"), mediaFilename);
        event.insert(QStringLiteral("msgtype"), msgtype);
        event.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
        event.insert(QStringLiteral("is_encrypted"), false);
        event.insert(QStringLiteral("is_decrypted"), false);
        event.insert(QStringLiteral("undecryptable"), false);
        event.insert(QStringLiteral("mentions_me"), false);
        event.insert(QStringLiteral("mentions_room"), false);
        event.insert(QStringLiteral("thread_root_id"), QString());
        event.insert(QStringLiteral("decrypted"), false);

        QJsonObject out;
        out.insert(QStringLiteral("type"), QStringLiteral("timeline_event"));
        out.insert(QStringLiteral("room_id"), QStringLiteral("!r:example.org"));
        out.insert(QStringLiteral("event"), event);
        return out;
    }

    struct Appended {
        QList<TimelineEvent> rows;
    };

    static void watch(RustSdkMatrixClient &client, Appended &sink)
    {
        QObject::connect(&client, &MatrixClient::eventAppended,
                         [&sink](const QString &, const TimelineEvent &row) {
                             sink.rows.append(row);
                         });
    }

    static TimelineEvent rowFor(RustSdkMatrixClient &client,
                                Appended &sink,
                                const QString &msgtype,
                                const QString &body,
                                const QString &mediaFilename,
                                const QString &eventId)
    {
        sink.rows.clear();
        client.handleRustEventForTest(
            syncedMessage(msgtype, body, mediaFilename, eventId));
        if (sink.rows.isEmpty()) {
            QTest::qFail(qPrintable(QStringLiteral(
                             "no row was appended for msgtype %1").arg(msgtype)),
                         __FILE__, __LINE__);
            return {};
        }
        return sink.rows.constLast();
    }

    static QList<QPair<QString, TimelineEvent::Type>> mediaCases()
    {
        return {
            { QStringLiteral("image"), TimelineEvent::Image },
            { QStringLiteral("video"), TimelineEvent::Video },
            { QStringLiteral("audio"), TimelineEvent::Audio },
            { QStringLiteral("file"), TimelineEvent::File },
            { QStringLiteral("location"), TimelineEvent::Location },
        };
    }
#endif

private slots:
    void aMediaRowKeepsItsKindThroughTheBridge()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        int n = 0;
        for (const auto &[msgtype, expected] : mediaCases()) {
            const bool carriesAFile = expected != TimelineEvent::Location;
            const TimelineEvent row =
                rowFor(client, sink, msgtype,
                       QStringLiteral("Big Ben, London"),
                       carriesAFile ? QStringLiteral("cat.png") : QString(),
                       QStringLiteral("$media%1").arg(++n));
            QVERIFY2(row.type == expected,
                     qPrintable(QStringLiteral("msgtype %1 became type %2")
                                    .arg(msgtype)
                                    .arg(int(row.type))));
        }
#endif
    }

    void everyNewRowKindStillProducesAPreviewLine()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        struct Case {
            QString msgtype;
            QString body;
            QString filename;
        };
        const QList<Case> cases{
            { QStringLiteral("image"), QStringLiteral("cat.png"),
              QStringLiteral("cat.png") },
            { QStringLiteral("video"), QStringLiteral("clip.mp4"),
              QStringLiteral("clip.mp4") },
            { QStringLiteral("audio"), QStringLiteral("song.ogg"),
              QStringLiteral("song.ogg") },
            { QStringLiteral("file"), QStringLiteral("here you go"),
              QStringLiteral("report.pdf") },
            { QStringLiteral("location"), QStringLiteral("Big Ben, London"),
              QString() },
        };

        int n = 0;
        for (const Case &c : cases) {
            const TimelineEvent row =
                rowFor(client, sink, c.msgtype, c.body, c.filename,
                       QStringLiteral("$preview%1").arg(++n));
            const QString summary = matrix::preview::oneLineSummary(row);
            QVERIFY2(!summary.isEmpty(),
                     qPrintable(QStringLiteral("%1 produced an empty preview")
                                    .arg(c.msgtype)));
        }

        const TimelineEvent file =
            rowFor(client, sink, QStringLiteral("file"),
                   QStringLiteral("here you go"),
                   QStringLiteral("report.pdf"), QStringLiteral("$f1"));
        QVERIFY(matrix::preview::oneLineSummary(file).contains(
            QStringLiteral("report.pdf")));

        const TimelineEvent place =
            rowFor(client, sink, QStringLiteral("location"),
                   QStringLiteral("Big Ben, London"), QString(),
                   QStringLiteral("$l1"));
        QCOMPARE(matrix::preview::oneLineSummary(place),
                 QStringLiteral("Big Ben, London"));
        QCOMPARE(place.body, QStringLiteral("Big Ben, London"));
#endif
    }

    void textLikeRowsAreUnchanged()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        const TimelineEvent text =
            rowFor(client, sink, QStringLiteral("text"),
                   QStringLiteral("hello"), QString(), QStringLiteral("$t1"));
        QCOMPARE(text.type, TimelineEvent::TextMessage);
        QCOMPARE(text.body, QStringLiteral("hello"));

        const TimelineEvent notice =
            rowFor(client, sink, QStringLiteral("notice"),
                   QStringLiteral("beep"), QString(), QStringLiteral("$t2"));
        QCOMPARE(notice.type, TimelineEvent::Notice);

        const TimelineEvent emote =
            rowFor(client, sink, QStringLiteral("emote"),
                   QStringLiteral("waves"), QString(), QStringLiteral("$t3"));
        QCOMPARE(emote.type, TimelineEvent::Emote);
#endif
    }

    void anUnknownKindIsStillAPlainMessageOnThisPath()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        const TimelineEvent row =
            rowFor(client, sink, QStringLiteral("com.example.custom"),
                   QStringLiteral("fallback text"), QString(),
                   QStringLiteral("$u1"));
        QCOMPARE(row.type, TimelineEvent::TextMessage);
        QCOMPARE(row.body, QStringLiteral("fallback text"));
#endif
    }

    void theEncryptedKindMapsToANoticeOnItsOwn()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        const TimelineEvent row =
            rowFor(client, sink, QStringLiteral("encrypted"),
                   QStringLiteral("placeholder words"), QString(),
                   QStringLiteral("$e0"));
        QCOMPARE(row.type, TimelineEvent::Notice);
        QVERIFY(!row.undecryptable);
#endif
    }

    void anUndecryptableEventStillReadsAsAPlaceholderNotice()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        QJsonObject out = syncedMessage(QStringLiteral("encrypted"), QString(),
                                        QString(), QStringLiteral("$e1"));
        QJsonObject event = out.value(QStringLiteral("event")).toObject();
        event.insert(QStringLiteral("is_encrypted"), true);
        event.insert(QStringLiteral("undecryptable"), true);
        event.insert(QStringLiteral("error_kind"), QStringLiteral("no_key"));
        out.insert(QStringLiteral("event"), event);
        client.handleRustEventForTest(out);

        QCOMPARE(sink.rows.size(), 1);
        const TimelineEvent row = sink.rows.constFirst();
        QCOMPARE(row.type, TimelineEvent::Notice);
        QVERIFY(row.undecryptable);
        QVERIFY(!row.body.isEmpty());
#endif
    }

    void aSyncedMessageCarriesItsPushVerdict()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        const auto sendWith = [&](const QString &eventId,
                                  const QJsonObject &verdict) {
            QJsonObject out = syncedMessage(QStringLiteral("text"),
                                            QStringLiteral("hello"), QString(),
                                            eventId);
            QJsonObject event = out.value(QStringLiteral("event")).toObject();
            for (auto it = verdict.constBegin(); it != verdict.constEnd(); ++it)
                event.insert(it.key(), it.value());
            out.insert(QStringLiteral("event"), event);
            sink.rows.clear();
            client.handleRustEventForTest(out);
        };

        sendWith(QStringLiteral("$pv1"),
                 QJsonObject{{QStringLiteral("push_notify"), false},
                             {QStringLiteral("push_highlight"), false}});
        QCOMPARE(sink.rows.size(), 1);
        QVERIFY2(sink.rows.constFirst().pushVerdict
                     == TimelineEvent::PushVerdict::Quiet,
                 "a quiet verdict was lost on the unopened-room path");

        sendWith(QStringLiteral("$pv2"),
                 QJsonObject{{QStringLiteral("push_notify"), true},
                             {QStringLiteral("push_highlight"), true}});
        QCOMPARE(sink.rows.size(), 1);
        QVERIFY(sink.rows.constFirst().pushVerdict
                == TimelineEvent::PushVerdict::Notify);
        QVERIFY(sink.rows.constFirst().pushHighlight);

        sendWith(QStringLiteral("$pv3"), QJsonObject{});
        QCOMPARE(sink.rows.size(), 1);
        QVERIFY(sink.rows.constFirst().pushVerdict
                == TimelineEvent::PushVerdict::Unknown);
#endif
    }

    void anEditIsNotANewMessage()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        const QString room = QStringLiteral("!r:example.org");
        QJsonObject listed;
        listed.insert(QStringLiteral("id"), room);
        listed.insert(QStringLiteral("name"), QStringLiteral("r"));
        QJsonObject snapshot;
        snapshot.insert(QStringLiteral("type"), QStringLiteral("room_snapshot"));
        snapshot.insert(QStringLiteral("rooms"), QJsonArray{ listed });
        client.handleRustEventForTest(snapshot);
        QCOMPARE(client.roomInfo(room).id, room);

        rowFor(client, sink, QStringLiteral("text"), QStringLiteral("hello"),
               QString(), QStringLiteral("$original"));
        QCOMPARE(client.roomInfo(room).lastMessagePreview, QStringLiteral("hello"));
        const QDateTime activity = client.roomInfo(room).lastActivity;
        QCOMPARE(client.timeline(room).size(), 1);

        QJsonObject out = syncedMessage(QStringLiteral("text"),
                                        QStringLiteral("* hello again"),
                                        QString(), QStringLiteral("$edit"));
        QJsonObject edit = out.value(QStringLiteral("event")).toObject();
        edit.insert(QStringLiteral("replaces_event_id"), QStringLiteral("$original"));
        edit.insert(QStringLiteral("timestamp_ms"), 1700000060000.0);
        out.insert(QStringLiteral("event"), edit);
        sink.rows.clear();
        client.handleRustEventForTest(out);

        QCOMPARE(sink.rows.size(), 1);
        QCOMPARE(client.roomInfo(room).lastMessagePreview, QStringLiteral("hello"));
        QCOMPARE(client.roomInfo(room).lastActivity, activity);
        QCOMPARE(client.timeline(room).size(), 0);

        client.m_timelineTracker.request(room);
        edit.insert(QStringLiteral("event_id"), QStringLiteral("$edit2"));
        out.insert(QStringLiteral("event"), edit);
        client.handleRustEventForTest(out);
        QCOMPARE(client.roomInfo(room).lastMessagePreview, QStringLiteral("hello"));
        QCOMPARE(client.roomInfo(room).lastActivity, activity);
        client.handleRustEventForTest(syncedMessage(
            QStringLiteral("text"), QStringLiteral("fresh"), QString(),
            QStringLiteral("$fresh")));
        QCOMPARE(client.roomInfo(room).lastMessagePreview, QStringLiteral("fresh"));
#endif
    }

    void anIgnoreChangeBlanksOnlyTheRefreshedRoomsUntilTheBackfillRefillsThem()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        const QString touched = QStringLiteral("!touched:example.org");
        const QString other = QStringLiteral("!other:example.org");
        QJsonArray rooms;
        for (const QString &id : { touched, other }) {
            QJsonObject listed;
            listed.insert(QStringLiteral("id"), id);
            listed.insert(QStringLiteral("name"), id);
            listed.insert(QStringLiteral("last_message_preview"), QStringLiteral("from bob"));
            listed.insert(QStringLiteral("last_activity_ms"), 1700000000000.0);
            rooms.append(listed);
        }
        QJsonObject snapshot;
        snapshot.insert(QStringLiteral("type"), QStringLiteral("room_snapshot"));
        snapshot.insert(QStringLiteral("rooms"), rooms);
        client.handleRustEventForTest(snapshot);
        QCOMPARE(client.roomInfo(touched).lastMessagePreview, QStringLiteral("from bob"));

        QJsonObject changed;
        changed.insert(QStringLiteral("type"), QStringLiteral("ignored_users_changed"));
        changed.insert(QStringLiteral("users"), QJsonArray{ QStringLiteral("@bob:example.org") });
        changed.insert(QStringLiteral("previews_refreshed_rooms"), QJsonArray{ touched });
        client.handleRustEventForTest(changed);
        QVERIFY2(client.roomInfo(touched).lastMessagePreview.isEmpty(),
                 "an ignored user's text stayed on a refreshed room");
        QCOMPARE(client.roomInfo(other).lastMessagePreview, QStringLiteral("from bob"));
        QCOMPARE(client.roomInfo(touched).lastActivity,
                 QDateTime::fromMSecsSinceEpoch(1700000000000, QTimeZone::UTC));

        QJsonObject backfill;
        backfill.insert(QStringLiteral("type"), QStringLiteral("room_preview_backfill"));
        backfill.insert(QStringLiteral("room_id"), touched);
        backfill.insert(QStringLiteral("preview"), QStringLiteral("from alice"));
        backfill.insert(QStringLiteral("last_activity_ms"), 1690000000000.0);
        client.handleRustEventForTest(backfill);
        QCOMPARE(client.roomInfo(touched).lastMessagePreview, QStringLiteral("from alice"));
#endif
    }

    void aRedeliveredEditIsForwardedOnce()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        const QString room = QStringLiteral("!r:example.org");

        rowFor(client, sink, QStringLiteral("text"), QStringLiteral("hello"),
               QString(), QStringLiteral("$original"));
        const auto editOf = [&](const QString &eventId, bool undecryptable) {
            QJsonObject out = syncedMessage(
                undecryptable ? QStringLiteral("encrypted") : QStringLiteral("text"),
                undecryptable ? QString() : QStringLiteral("* hello again"),
                QString(), eventId);
            QJsonObject edit = out.value(QStringLiteral("event")).toObject();
            edit.insert(QStringLiteral("replaces_event_id"), QStringLiteral("$original"));
            if (undecryptable) {
                edit.insert(QStringLiteral("is_encrypted"), true);
                edit.insert(QStringLiteral("undecryptable"), true);
                edit.insert(QStringLiteral("error_kind"), QStringLiteral("no_key"));
            }
            out.insert(QStringLiteral("event"), edit);
            return out;
        };

        int checked = 0;
        for (const bool undecryptable : { false, true }) {
            const QString id = undecryptable ? QStringLiteral("$utd-edit")
                                             : QStringLiteral("$edit");
            sink.rows.clear();
            client.handleRustEventForTest(editOf(id, undecryptable));
            QCOMPARE(sink.rows.size(), 1);
            client.handleRustEventForTest(editOf(id, undecryptable));
            QVERIFY2(sink.rows.size() == 1,
                     qPrintable(QStringLiteral("%1 was forwarded %2 times")
                                    .arg(id).arg(sink.rows.size())));
            QCOMPARE(client.timeline(room).size(), 0);
            ++checked;
        }
        QCOMPARE(checked, 2);

        sink.rows.clear();
        client.handleRustEventForTest(editOf(QStringLiteral("$edit-next"), false));
        QCOMPARE(sink.rows.size(), 1);
#endif
    }
};

QTEST_MAIN(SyncMessageRowTest)
#include "SyncMessageRowTest.moc"
