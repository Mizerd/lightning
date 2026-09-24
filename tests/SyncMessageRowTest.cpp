// A synced media message (m.image / m.video / m.audio / m.file / m.location)
// in a room with no open timeline must become a row of the right kind:
// `typeFromString` in RustSdkMatrixClient.cpp must map every kind, not only
// notice/emote. The Rust half is covered by `message_row_kind_tests` in
// rust/src/lib.rs.
//
// `everyNewRowKindStillProducesAPreviewLine` checks the string each consumer
// builds from the row, which is what the user actually reads.

#include "app/SettingsManager.h"
#include "matrix/EventPreview.h"
#include "matrix/TimelineEvent.h"

#include <QJsonObject>
#include <QList>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

class SyncMessageRowTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    /// A hand-maintained copy of the payload rust/src/lib.rs enqueues for one
    /// synced message; it cannot see a rename on the Rust side. Every field
    /// the handler reads is present.
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
        // The prep+5 compatibility field the handler still falls back to.
        event.insert(QStringLiteral("decrypted"), false);

        QJsonObject out;
        out.insert(QStringLiteral("type"), QStringLiteral("timeline_event"));
        out.insert(QStringLiteral("room_id"), QStringLiteral("!r:example.org"));
        out.insert(QStringLiteral("event"), event);
        return out;
    }

    /// A direct connection rather than a QSignalSpy, so the row arrives as
    /// itself and never has to survive a QVariant round trip.
    struct Appended {
        QList<TimelineEvent> rows;
    };

    /// Takes the sink by reference and keeps its address, so callers declare
    /// the sink before the client and the client is destroyed first.
    static void watch(RustSdkMatrixClient &client, Appended &sink)
    {
        QObject::connect(&client, &MatrixClient::eventAppended,
                         [&sink](const QString &, const TimelineEvent &row) {
                             sink.rows.append(row);
                         });
    }

    /// Drive the real dispatcher and return the row it appended. Fails when
    /// nothing arrived, rather than returning a default-constructed row.
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
            // qFail rather than QVERIFY2: the macros expand to a bare
            // `return;`, which cannot compile in a function returning a row.
            QTest::qFail(qPrintable(QStringLiteral(
                             "no row was appended for msgtype %1").arg(msgtype)),
                         __FILE__, __LINE__);
            return {};
        }
        return sink.rows.constLast();
    }

    /// The media kinds, with the msgtype string the bridge emits and the row
    /// kind it must become.
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
    /// Every media kind keeps its row type through the bridge, rather than
    /// reading as TextMessage.
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
            // A location carries no filename on the wire, so do not feed it
            // one here either: the fixture must not compose a shape the
            // bridge cannot send, even in a case that asserts only the type.
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

    /// Every kind must still produce a preview line. `oneLineSummary` reads
    /// `mediaFilename` for image/video/audio/file and `body` for a location,
    /// so a location's words must arrive as its body. The producer half is
    /// pinned by `a_location_and_every_text_row_carry_no_filename` in
    /// rust/src/lib.rs.
    void everyNewRowKindStillProducesAPreviewLine()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Appended sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        // What the bridge really sends per kind: a location carries its words
        // as the body and NO filename; the other four carry both.
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

        // And the two that carry real information must carry it, not just a
        // non-empty placeholder: the file's real name rather than its
        // caption, and the location's own words.
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

    /// The kinds that always worked must keep working, and a body must still
    /// cross for them.
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

    /// A kind this path has no row for stays a plain message rather than an
    /// Unknown row (unlike the live-timeline ingest).
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

    /// `"encrypted"` maps to a Notice. Asserted without the undecryptable
    /// flag, because the placeholder branch overrides the type whenever the
    /// flag is set.
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

    /// And the placeholder branch itself: an undecryptable event keeps its
    /// honest text and its Notice row.
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
};

QTEST_MAIN(SyncMessageRowTest)
#include "SyncMessageRowTest.moc"
