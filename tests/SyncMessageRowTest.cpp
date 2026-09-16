// AN IMAGE SENT TO A ROOM WITH NO TIMELINE OPEN PRODUCED NOTHING AT ALL.
//
// Two mappings translate a synced message into a row kind, and both were
// wrong for media in the same way:
//
//   * rust/src/lib.rs's live-sync handler matched `Text | Notice | Emote` and
//     `_ => return`, so an m.image / m.video / m.audio / m.file / m.location
//     never became a `timeline_event` at all. No notification, no Activity
//     row, nothing.
//   * `RustSdkMatrixClient.cpp`'s own `typeFromString` knew "notice" and
//     "emote" and called everything else a plain TextMessage — a private copy
//     of the mapping `RustTimelineIngest` already had complete, so even a
//     media row that DID arrive lost its kind.
//
// Everything downstream had handled media correctly for versions:
// NotificationManager says "Sent an image" from the row TYPE,
// EventPreview::oneLineSummary degrades to "Image", ActivityModel has an icon
// per kind. Only the two mappings in front of them did not.
//
// FAIL-ON-OLD: restore `typeFromString`'s body to
// `return TimelineEvent::TextMessage;` for anything but notice/emote and
// `aMediaRowKeepsItsKindThroughTheBridge` reads TextMessage for every media
// kind. (The Rust half is covered by `message_row_kind_tests` in
// rust/src/lib.rs; it cannot be reached from C++.)
//
// AND THE CASE A REVIEW ADDED, because the first cut of this suite stopped at
// `TimelineEvent::type` and a defect walked straight through that gap: a row
// kind is only half the story, and what the user actually reads is the STRING
// each consumer builds from it. `everyNewRowKindStillProducesAPreviewLine`
// asserts that, and it is what catches a body routed into the wrong field.

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
    /// A HAND-MAINTAINED COPY of the payload rust/src/lib.rs enqueues for one
    /// synced message. It is not derived from the Rust source and cannot see
    /// a rename there — say so plainly rather than claiming a guarantee this
    /// fixture does not provide. Every field the handler reads is present, so
    /// a field DROPPED on the Rust side still shows up here as a behaviour
    /// change in whichever case depends on it.
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

    /// Takes the sink BY REFERENCE and keeps it: the lambda holds its
    /// address, so a sink created inside this helper and returned by value
    /// would leave every capture dangling. Callers declare the sink BEFORE
    /// the client, so the client is destroyed first and no connection can
    /// outlive what it writes into.
    static void watch(RustSdkMatrixClient &client, Appended &sink)
    {
        QObject::connect(&client, &MatrixClient::eventAppended,
                         [&sink](const QString &, const TimelineEvent &row) {
                             sink.rows.append(row);
                         });
    }

    /// Drive the real dispatcher and return the row it appended.
    ///
    /// The regression under test is literally "no row is produced", so this
    /// FAILS when nothing arrived rather than handing back a
    /// default-constructed row that a type comparison would happen to reject.
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

    /// The five kinds the live-sync handler used to drop, with the msgtype
    /// string the bridge emits and the row kind it must become.
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
    /// THE DEFECT. Every media kind read as TextMessage, so every media
    /// message in a background room notified as though it were text.
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

    /// THE CASE A REVIEW ADDED, AND IT CAUGHT A REAL DEFECT.
    ///
    /// A row kind is not the thing the user reads. `oneLineSummary` is what
    /// writes the room-list line, and it reads a DIFFERENT field per kind:
    /// `mediaFilename` for image/video/audio/file, and — because it has no
    /// Location case at all — `body` for a location. An earlier cut of this
    /// change routed every media body into `media_filename`, which left a
    /// location's own words nowhere and wiped the room's preview line to
    /// empty. Nothing that stopped at `type` could see it.
    ///
    /// WHERE THE FAIL-ON-OLD FOR THAT DEFECT ACTUALLY LIVES, stated exactly
    /// because it is NOT here: this suite drives the C++ dispatcher with a
    /// payload the fixture composes, so it cannot see a routing change on the
    /// Rust side at all. The producer half is pinned by
    /// `a_location_and_every_text_row_carry_no_filename` in rust/src/lib.rs —
    /// make `media_filename_for_kind` answer `body` for "location" and that
    /// one case fails, alone. What THIS case pins is the other half, the half
    /// that makes the routing matter: given a location whose words arrive as
    /// the body, every one of these kinds must still produce a line a reader
    /// can see. Neither half is sufficient; the defect needed both to be
    /// obvious.
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

    /// A kind this path has no row for stays a plain message rather than
    /// becoming an Unknown row — the long-standing answer here, and
    /// deliberately NOT the live-timeline ingest's, which renders a real
    /// timeline and must not let an unrecognised row masquerade as text.
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

    /// `"encrypted"` is the one reachable string whose shared-mapping answer
    /// this change moved (TextMessage -> Notice), so it is asserted WITHOUT
    /// the undecryptable flag — the placeholder branch below overrides the
    /// type unconditionally, so a case that sets the flag cannot tell the
    /// mapping's answer from the override's.
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
