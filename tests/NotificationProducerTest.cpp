// The Rust backend's notification-path signals, driven through the real
// dispatcher with hand-built bridge payloads (no FFI handle, no homeserver).
// 2026-10-15 notification sweep, package P3. Each case names the finding it
// pins and what the old code did instead.
//
// The payload shapes are hand-maintained copies of what rust/src/lib.rs and
// rust/src/timeline.rs enqueue; they cannot see a rename on the Rust side.

#include "app/SettingsManager.h"
#include "matrix/CallSignal.h"
#include "matrix/TimelineEvent.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QtTest>

#ifdef ENABLE_RUST_SDK_BACKEND
#include "matrix/RustSdkMatrixClient.h"
#endif

class NotificationProducerTest : public QObject
{
    Q_OBJECT

#ifdef ENABLE_RUST_SDK_BACKEND
private:
    static inline const QString kRoom = QStringLiteral("!r:example.org");

    /// Everything the notification consumers and the timeline model would
    /// hear, recorded by direct connections so rows never round-trip through
    /// QVariant. Declare it before the client so the client dies first.
    struct Sink {
        QList<TimelineEvent> appended;
        QList<TimelineEvent> observed;
        QList<TimelineEvent> decrypted;
        QStringList redacted;
        QList<QPair<int, TimelineEvent>> insertedAt;
        QList<QPair<QString, int>> highlightGaps;
        QStringList receiptsAccepted;
        QStringList receiptsFailed;
        int seedFailed = 0;
        QList<QVariantList> seeds;
        QList<CallSignal> calls;
        struct Reaction {
            QString id;
            bool targetIsOwn = false;
            bool backlog = false;
        };
        QList<Reaction> reactions;
    };

    static void watch(RustSdkMatrixClient &client, Sink &sink)
    {
        QObject::connect(&client, &MatrixClient::eventAppended,
                         [&sink](const QString &, const TimelineEvent &e) {
                             sink.appended.append(e);
                         });
        QObject::connect(&client, &MatrixClient::liveEventObserved,
                         [&sink](const QString &room, const TimelineEvent &e) {
                             QCOMPARE(room, e.roomId);
                             sink.observed.append(e);
                         });
        QObject::connect(&client, &MatrixClient::eventDecrypted,
                         [&sink](const QString &, const TimelineEvent &e) {
                             sink.decrypted.append(e);
                         });
        QObject::connect(&client, &MatrixClient::eventRedacted,
                         [&sink](const QString &, const QString &id) {
                             sink.redacted.append(id);
                         });
        QObject::connect(&client, &MatrixClient::eventInsertedAt,
                         [&sink](const QString &, int index, const TimelineEvent &e) {
                             sink.insertedAt.append({ index, e });
                         });
        QObject::connect(&client, &MatrixClient::roomHighlightsUnnotified,
                         [&sink](const QString &room, int count) {
                             sink.highlightGaps.append({ room, count });
                         });
        QObject::connect(&client, &MatrixClient::readReceiptAccepted,
                         [&sink](const QString &, const QString &id) {
                             sink.receiptsAccepted.append(id);
                         });
        QObject::connect(&client, &MatrixClient::readReceiptFailed,
                         [&sink](const QString &, const QString &id) {
                             sink.receiptsFailed.append(id);
                         });
        QObject::connect(&client, &MatrixClient::activitySeedFailed,
                         [&sink] { ++sink.seedFailed; });
        QObject::connect(&client, &MatrixClient::activitySeedReceived,
                         [&sink](const QVariantList &entries) {
                             sink.seeds.append(entries);
                         });
        QObject::connect(&client, &MatrixClient::callSignalReceived,
                         [&sink](const CallSignal &signal) {
                             sink.calls.append(signal);
                         });
        QObject::connect(&client, &MatrixClient::reactionEventReceived,
                         [&sink](const QString &, const QString &id,
                                 const QString &, const QString &,
                                 const QString &, qint64, bool own, bool backlog) {
                             sink.reactions.append({ id, own, backlog });
                         });
    }

    /// The inner `event` of a sync-handler `timeline_event`.
    static QJsonObject message(const QString &eventId, const QString &body)
    {
        QJsonObject event;
        event.insert(QStringLiteral("event_id"), eventId);
        event.insert(QStringLiteral("sender"), QStringLiteral("@bob:example.org"));
        event.insert(QStringLiteral("body"), body);
        event.insert(QStringLiteral("msgtype"), QStringLiteral("text"));
        event.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
        event.insert(QStringLiteral("is_encrypted"), false);
        event.insert(QStringLiteral("is_decrypted"), false);
        event.insert(QStringLiteral("undecryptable"), false);
        event.insert(QStringLiteral("mentions_me"), false);
        event.insert(QStringLiteral("mentions_room"), false);
        event.insert(QStringLiteral("thread_root_id"), QString());
        return event;
    }

    static QJsonObject undecryptable(const QString &eventId)
    {
        QJsonObject event = message(eventId, QString());
        event.insert(QStringLiteral("msgtype"), QStringLiteral("encrypted"));
        event.insert(QStringLiteral("is_encrypted"), true);
        event.insert(QStringLiteral("undecryptable"), true);
        return event;
    }

    static QJsonObject envelope(const QString &type, const QJsonObject &event,
                                const QString &room = kRoom)
    {
        QJsonObject out;
        out.insert(QStringLiteral("type"), type);
        out.insert(QStringLiteral("room_id"), room);
        out.insert(QStringLiteral("event"), event);
        return out;
    }

    static QJsonObject roomJson(const QString &id, int highlights)
    {
        QJsonObject room;
        room.insert(QStringLiteral("id"), id);
        room.insert(QStringLiteral("name"), QStringLiteral("r"));
        room.insert(QStringLiteral("membership"), QStringLiteral("joined"));
        room.insert(QStringLiteral("unread_count"), highlights);
        room.insert(QStringLiteral("highlight_count"), highlights);
        return room;
    }

    static void listRoom(RustSdkMatrixClient &client, int highlights)
    {
        QJsonObject snapshot;
        snapshot.insert(QStringLiteral("type"), QStringLiteral("room_snapshot"));
        snapshot.insert(QStringLiteral("rooms"), QJsonArray{ roomJson(kRoom, highlights) });
        client.handleRustEventForTest(snapshot);
    }

    /// A live-timeline item as rust/src/timeline.rs serialises one.
    static QJsonObject item(const QString &itemId, const QString &eventId,
                            const QString &body)
    {
        QJsonObject item;
        item.insert(QStringLiteral("item_id"), itemId);
        item.insert(QStringLiteral("kind"), QStringLiteral("event"));
        item.insert(QStringLiteral("event_id"), eventId);
        item.insert(QStringLiteral("sender"), QStringLiteral("@bob:example.org"));
        item.insert(QStringLiteral("msgtype"), QStringLiteral("text"));
        item.insert(QStringLiteral("body"), body);
        item.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
        return item;
    }

    static QJsonObject failedEcho(const QString &txn)
    {
        QJsonObject echo = item(txn, QString(), QStringLiteral("mine"));
        echo.remove(QStringLiteral("event_id"));
        echo.insert(QStringLiteral("transaction_id"), txn);
        echo.insert(QStringLiteral("sender"), QStringLiteral("@me:example.org"));
        echo.insert(QStringLiteral("is_local_echo"), true);
        echo.insert(QStringLiteral("send_state"), QStringLiteral("failed"));
        return echo;
    }

    /// Opens `kRoom`'s live timeline at generation 1 with `items`.
    static void openRoom(RustSdkMatrixClient &client, const QJsonArray &items)
    {
        client.m_timelineTracker.request(kRoom);
        QJsonObject reset;
        reset.insert(QStringLiteral("type"), QStringLiteral("timeline_reset"));
        reset.insert(QStringLiteral("room_id"), kRoom);
        reset.insert(QStringLiteral("room_generation"), 1);
        reset.insert(QStringLiteral("items"), items);
        client.handleRustEventForTest(reset);
        QCOMPARE(client.m_timelineTracker.activeRoom(), kRoom);
    }

    static QJsonObject diff(const QString &op, int index, const QJsonObject &item)
    {
        QJsonObject d;
        d.insert(QStringLiteral("type"), QStringLiteral("timeline_diff"));
        d.insert(QStringLiteral("room_id"), kRoom);
        d.insert(QStringLiteral("room_generation"), 1);
        d.insert(QStringLiteral("op"), op);
        if (index >= 0)
            d.insert(QStringLiteral("index"), index);
        d.insert(QStringLiteral("item"), item);
        return d;
    }
#endif

private slots:
    // Findings 45/127. Old code: handleTimelineEvent returned for the open
    // room after a preview update, so a thread reply there reached nobody.
    void aThreadReplyInTheOpenRoomIsObservedAndNeverAppended()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        listRoom(client, 0);
        client.m_timelineTracker.request(kRoom);

        QJsonObject reply = message(QStringLiteral("$reply"), QStringLiteral("@me hi"));
        reply.insert(QStringLiteral("thread_root_id"), QStringLiteral("$root"));
        reply.insert(QStringLiteral("mentions_me"), true);
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), reply));

        QVERIFY2(sink.appended.isEmpty(),
                 "a thread reply reached eventAppended, i.e. the main timeline");
        QCOMPARE(sink.observed.size(), 1);
        QCOMPARE(sink.observed.first().eventId, QStringLiteral("$reply"));
        QCOMPARE(sink.observed.first().threadRootId, QStringLiteral("$root"));
        QVERIFY(sink.observed.first().mentionsMe);
        QVERIFY(client.timeline(kRoom).isEmpty());

        // Control: an ordinary message in the open room is the live timeline's
        // to announce, so neither signal fires for it here.
        client.handleRustEventForTest(envelope(
            QStringLiteral("timeline_event"),
            message(QStringLiteral("$plain"), QStringLiteral("plain"))));
        QCOMPARE(sink.observed.size(), 1);
        QVERIFY(sink.appended.isEmpty());
#endif
    }

    // Findings 9/52, 8, 44/120: the sync payload's facts ride on the row.
    void aSyncedEditCarriesItsFactsAndAReplyResolvesItsTarget()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        listRoom(client, 0);

        QJsonObject mine = message(QStringLiteral("$mine"), QStringLiteral("hello"));
        mine.insert(QStringLiteral("sender"), QStringLiteral("@me:example.org"));
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), mine));

        QJsonObject reply = message(QStringLiteral("$reply"), QStringLiteral("yes"));
        reply.insert(QStringLiteral("in_reply_to"), QStringLiteral("$mine"));
        reply.insert(QStringLiteral("backlog"), true);
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), reply));
        QCOMPARE(sink.appended.size(), 2);
        QCOMPARE(sink.appended.at(1).replyToEventId, QStringLiteral("$mine"));
        QCOMPARE(sink.appended.at(1).replyToSenderId, QStringLiteral("@me:example.org"));
        QVERIFY(sink.appended.at(1).backlog);

        QJsonObject edit = message(QStringLiteral("$edit"), QStringLiteral("* hello!"));
        edit.insert(QStringLiteral("replaces_event_id"), QStringLiteral("$mine"));
        edit.insert(QStringLiteral("new_body"), QStringLiteral("hello!"));
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), edit));
        QCOMPARE(sink.appended.size(), 3);
        QVERIFY2(sink.appended.at(2).isEdit, "an edit went out unmarked");
        QCOMPARE(sink.appended.at(2).replacesEventId, QStringLiteral("$mine"));
        QCOMPARE(sink.appended.at(2).editNewBody, QStringLiteral("hello!"));
#endif
    }

    // Finding 53: a forwarded poll start notifies once and never enters the
    // mirror as an answerless poll card.
    void aSyncedPollIsForwardedOnceAndNeverMirrored()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        listRoom(client, 0);

        QJsonObject poll = message(QStringLiteral("$poll"), QStringLiteral("Lunch?"));
        poll.insert(QStringLiteral("msgtype"), QStringLiteral("poll"));
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), poll));
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), poll));
        QCOMPARE(sink.appended.size(), 1);
        QCOMPARE(sink.appended.first().type, TimelineEvent::Poll);
        QVERIFY(client.timeline(kRoom).isEmpty());
#endif
    }

    // Findings 6/46/124. Old code: nothing handled `event_decrypted`, and the
    // mirror kept the placeholder for good.
    void aLateDecryptionIsAnnouncedOnceAndOnlyForWhatArrived()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        listRoom(client, 0);

        client.handleRustEventForTest(envelope(
            QStringLiteral("timeline_event"), undecryptable(QStringLiteral("$e"))));
        QCOMPARE(sink.appended.size(), 1);
        QVERIFY(sink.appended.first().undecryptable);

        QJsonObject readable = message(QStringLiteral("$e"), QStringLiteral("@me look"));
        readable.insert(QStringLiteral("is_encrypted"), true);
        readable.insert(QStringLiteral("is_decrypted"), true);
        readable.insert(QStringLiteral("mentions_me"), true);
        readable.insert(QStringLiteral("push_notify"), true);
        readable.insert(QStringLiteral("push_highlight"), true);
        client.handleRustEventForTest(envelope(QStringLiteral("event_decrypted"), readable));
        QCOMPARE(sink.decrypted.size(), 1);
        const TimelineEvent &decrypted = sink.decrypted.first();
        QCOMPARE(decrypted.eventId, QStringLiteral("$e"));
        QCOMPARE(decrypted.roomId, kRoom);
        QVERIFY(!decrypted.undecryptable);
        QVERIFY(decrypted.mentionsMe);
        QVERIFY(decrypted.pushHighlight);
        QVERIFY(!decrypted.backlog);
        // The mirror no longer holds the placeholder.
        QCOMPARE(client.timeline(kRoom).size(), 1);
        QVERIFY(!client.timeline(kRoom).first().undecryptable);
        // Never twice.
        client.handleRustEventForTest(envelope(QStringLiteral("event_decrypted"), readable));
        QCOMPARE(sink.decrypted.size(), 1);

        // Never for an event this session did not see arrive undecryptable.
        QJsonObject stranger = readable;
        stranger.insert(QStringLiteral("event_id"), QStringLiteral("$stranger"));
        client.handleRustEventForTest(envelope(QStringLiteral("event_decrypted"), stranger));
        QCOMPARE(sink.decrypted.size(), 1);

        // History stays history once readable.
        QJsonObject old = undecryptable(QStringLiteral("$old"));
        old.insert(QStringLiteral("backlog"), true);
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), old));
        QJsonObject oldReadable = readable;
        oldReadable.insert(QStringLiteral("event_id"), QStringLiteral("$old"));
        client.handleRustEventForTest(envelope(QStringLiteral("event_decrypted"), oldReadable));
        QCOMPARE(sink.decrypted.size(), 2);
        QVERIFY(sink.decrypted.at(1).backlog);
#endif
    }

    // Finding 1. Old code: the Rust backend never emitted eventRedacted.
    void aRedactionIsAnnouncedWithoutGettingAheadOfTheOpenTimeline()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        listRoom(client, 0);

        client.handleRustEventForTest(envelope(
            QStringLiteral("timeline_event"),
            message(QStringLiteral("$secret"), QStringLiteral("private"))));
        QJsonObject redaction;
        redaction.insert(QStringLiteral("type"), QStringLiteral("event_redacted"));
        redaction.insert(QStringLiteral("room_id"), kRoom);
        redaction.insert(QStringLiteral("redacts"), QStringLiteral("$secret"));
        redaction.insert(QStringLiteral("redaction_event_id"), QStringLiteral("$r1"));
        client.handleRustEventForTest(redaction);
        QCOMPARE(sink.redacted, QStringList{ QStringLiteral("$secret") });
        QVERIFY(client.timeline(kRoom).first().redacted);
        QVERIFY(client.timeline(kRoom).first().body.isEmpty());

        // In the open room a row is the SDK's to redact (by its own Set), but a
        // reaction has no row and is announced at once.
        openRoom(client, QJsonArray{ item(QStringLiteral("i0"), QStringLiteral("$row"),
                                          QStringLiteral("row")) });
        redaction.insert(QStringLiteral("redacts"), QStringLiteral("$row"));
        client.handleRustEventForTest(redaction);
        QCOMPARE(sink.redacted.size(), 1);
        redaction.insert(QStringLiteral("redacts"), QStringLiteral("$reaction"));
        client.handleRustEventForTest(redaction);
        QCOMPARE(sink.redacted.last(), QStringLiteral("$reaction"));

        // ... and the SDK's Set is what announces the row.
        QJsonObject gone = item(QStringLiteral("i0"), QStringLiteral("$row"), QString());
        gone.insert(QStringLiteral("redacted"), true);
        client.handleRustEventForTest(diff(QStringLiteral("set"), 0, gone));
        QCOMPARE(sink.redacted.last(), QStringLiteral("$row"));
        QCOMPARE(sink.redacted.size(), 3);
#endif
    }

    // Finding 151 and the open-timeline half of 6/46/124. Old code: an insert
    // above a failed send produced only eventInsertedAt, and a Set that
    // decrypted a row nothing at all for notifications.
    void theOpenTimelinesInsertsAndSetsReachTheNotificationPath()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        listRoom(client, 0);
        openRoom(client, QJsonArray{
            item(QStringLiteral("i0"), QStringLiteral("$a"), QStringLiteral("a")),
            failedEcho(QStringLiteral("txn1")),
        });

        client.handleRustEventForTest(diff(
            QStringLiteral("insert"), 1,
            item(QStringLiteral("i1"), QStringLiteral("$new"), QStringLiteral("new"))));
        // The model still hears it as an insert ...
        QCOMPARE(sink.insertedAt.size(), 1);
        QCOMPARE(sink.insertedAt.first().first, 1);
        // ... and the notification path as a live arrival, never as an append.
        QVERIFY(sink.appended.isEmpty());
        QCOMPARE(sink.observed.size(), 1);
        QCOMPARE(sink.observed.first().eventId, QStringLiteral("$new"));

        // An undecryptable live append, then its decryption by Set.
        QJsonObject utd = item(QStringLiteral("i2"), QStringLiteral("$utd"), QString());
        utd.insert(QStringLiteral("undecryptable"), true);
        utd.insert(QStringLiteral("is_encrypted"), true);
        client.handleRustEventForTest(diff(QStringLiteral("push_back"), -1, utd));
        QCOMPARE(sink.appended.size(), 1);
        const int row = client.timeline(kRoom).size() - 1;
        QJsonObject readable = item(QStringLiteral("i2"), QStringLiteral("$utd"),
                                    QStringLiteral("now readable"));
        readable.insert(QStringLiteral("is_encrypted"), true);
        readable.insert(QStringLiteral("is_decrypted"), true);
        client.handleRustEventForTest(diff(QStringLiteral("set"), row, readable));
        QCOMPARE(sink.decrypted.size(), 1);
        QCOMPARE(sink.decrypted.first().eventId, QStringLiteral("$utd"));
        QCOMPARE(sink.decrypted.first().roomId, kRoom);
        // A second Set of the same readable row is not a second decryption.
        client.handleRustEventForTest(diff(QStringLiteral("set"), row, readable));
        QCOMPARE(sink.decrypted.size(), 1);
#endif
    }

    // Finding 18. Old code: a failed seed emitted an empty list, which reads
    // as "no highlights" and was never retried.
    void aFailedSeedIsNotAnEmptySeed()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        QJsonObject failed;
        failed.insert(QStringLiteral("type"), QStringLiteral("activity_seed"));
        failed.insert(QStringLiteral("ok"), false);
        failed.insert(QStringLiteral("entries"), QJsonArray{});
        client.handleRustEventForTest(failed);
        QCOMPARE(sink.seedFailed, 1);
        QVERIFY2(sink.seeds.isEmpty(), "a failed seed was reported as an empty one");

        // Finding 17 (C++ half): the msgtype passes through.
        QJsonObject entry;
        entry.insert(QStringLiteral("event_id"), QStringLiteral("$h"));
        entry.insert(QStringLiteral("room_id"), kRoom);
        entry.insert(QStringLiteral("msgtype"), QStringLiteral("m.image"));
        QJsonObject ok;
        ok.insert(QStringLiteral("type"), QStringLiteral("activity_seed"));
        ok.insert(QStringLiteral("ok"), true);
        ok.insert(QStringLiteral("entries"), QJsonArray{ entry });
        client.handleRustEventForTest(ok);
        QCOMPARE(sink.seeds.size(), 1);
        QCOMPARE(sink.seeds.first().first().toMap()
                     .value(QStringLiteral("msgtype")).toString(),
                 QStringLiteral("m.image"));
        QCOMPARE(sink.seedFailed, 1);
#endif
    }

    // Finding 70. Old code: neither outcome reached anyone, so a failed
    // receipt was never retried.
    void aReceiptReportsItsOwnOutcome()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        client.m_lastReceiptSent.insert(kRoom, QStringLiteral("$seen"));
        QJsonObject accepted;
        accepted.insert(QStringLiteral("type"), QStringLiteral("read_marker_advanced"));
        accepted.insert(QStringLiteral("room_id"), kRoom);
        client.handleRustEventForTest(accepted);
        QCOMPARE(sink.receiptsAccepted, QStringList{ QStringLiteral("$seen") });

        client.m_lastReceiptSent.insert(kRoom, QStringLiteral("$next"));
        QJsonObject failed;
        failed.insert(QStringLiteral("type"), QStringLiteral("room_action_error"));
        failed.insert(QStringLiteral("action"), QStringLiteral("read_receipt"));
        failed.insert(QStringLiteral("room_id"), kRoom);
        failed.insert(QStringLiteral("reason"), QStringLiteral("transport"));
        client.handleRustEventForTest(failed);
        QCOMPARE(sink.receiptsFailed, QStringList{ QStringLiteral("$next") });
        // Forgotten, so the same event can be sent again.
        QVERIFY(!client.m_lastReceiptSent.contains(kRoom));

        // With no bridge at all the receipt fails, queued (never re-entering
        // the caller from inside its own send).
        client.sendReadReceipt(kRoom, QStringLiteral("$offline"));
        QCOMPARE(sink.receiptsFailed.size(), 1);
        QTRY_COMPARE(sink.receiptsFailed.size(), 2);
        QCOMPARE(sink.receiptsFailed.last(), QStringLiteral("$offline"));
#endif
    }

    // Finding 75 (C++ half). Old code: notification_type and m.mentions were
    // never read, so every announcement looked like a ring.
    void aCallAnnouncementCarriesItsTypeAndAddressees()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        QJsonObject notice;
        notice.insert(QStringLiteral("type"), QStringLiteral("call_rtc_notification"));
        notice.insert(QStringLiteral("room_id"), kRoom);
        notice.insert(QStringLiteral("event_id"), QStringLiteral("$call"));
        notice.insert(QStringLiteral("sender"), QStringLiteral("@bob:example.org"));
        notice.insert(QStringLiteral("notification_type"), QStringLiteral("ring"));
        notice.insert(QStringLiteral("mentions_user_ids"),
                      QJsonArray{ QStringLiteral("@me:example.org"),
                                  QStringLiteral("not-a-user-id") });
        notice.insert(QStringLiteral("mentions_room"), false);
        client.handleRustEventForTest(notice);
        QCOMPARE(sink.calls.size(), 1);
        QCOMPARE(sink.calls.first().notificationType, QStringLiteral("ring"));
        QCOMPARE(sink.calls.first().mentionUserIds,
                 QStringList{ QStringLiteral("@me:example.org") });
        QVERIFY(!sink.calls.first().mentionsRoom);
        // Absent: not addressed to us by Rust's whole-list check.
        QVERIFY(!sink.calls.first().mentionsMe);

        // Rust's verdict on the WHOLE m.mentions list reaches the signal even
        // when the bounded id list does not name us.
        notice.insert(QStringLiteral("event_id"), QStringLiteral("$call-me"));
        notice.insert(QStringLiteral("mentions_user_ids"), QJsonArray{});
        notice.insert(QStringLiteral("mentions_me"), true);
        client.handleRustEventForTest(notice);
        QCOMPARE(sink.calls.size(), 2);
        QVERIFY(sink.calls.last().mentionsMe);
        QVERIFY(sink.calls.last().mentionUserIds.isEmpty());
        notice.remove(QStringLiteral("mentions_me"));

        // Outside the closed set: not stated.
        notice.insert(QStringLiteral("event_id"), QStringLiteral("$call2"));
        notice.insert(QStringLiteral("notification_type"), QStringLiteral("RING!"));
        notice.insert(QStringLiteral("mentions_room"), true);
        client.handleRustEventForTest(notice);
        QCOMPARE(sink.calls.size(), 3);
        QVERIFY(sink.calls.last().notificationType.isEmpty());
        QVERIFY(sink.calls.last().mentionsRoom);
#endif
    }

    // Finding 156 (C++ half).
    void aReactionCarriesWhetherItsTargetIsOurs()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);

        QJsonObject reaction;
        reaction.insert(QStringLiteral("type"), QStringLiteral("reaction_event"));
        reaction.insert(QStringLiteral("room_id"), kRoom);
        reaction.insert(QStringLiteral("event_id"), QStringLiteral("$like"));
        reaction.insert(QStringLiteral("target_event_id"), QStringLiteral("$old"));
        reaction.insert(QStringLiteral("sender"), QStringLiteral("@bob:example.org"));
        reaction.insert(QStringLiteral("key"), QStringLiteral("+1"));
        reaction.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
        reaction.insert(QStringLiteral("target_is_own"), true);
        reaction.insert(QStringLiteral("backlog"), true);
        client.handleRustEventForTest(reaction);
        QCOMPARE(sink.reactions.size(), 1);
        QVERIFY(sink.reactions.first().targetIsOwn);
        QVERIFY(sink.reactions.first().backlog);

        // An older bridge: not known, not history.
        reaction.remove(QStringLiteral("target_is_own"));
        reaction.remove(QStringLiteral("backlog"));
        client.handleRustEventForTest(reaction);
        QVERIFY(!sink.reactions.last().targetIsOwn);
        QVERIFY(!sink.reactions.last().backlog);
#endif
    }

    // Finding 121. Old code: a highlight count rising with no event delivered
    // told nobody anything.
    void aHighlightNobodyWasToldAboutIsReportedOnce()
    {
#ifndef ENABLE_RUST_SDK_BACKEND
        QSKIP("needs the Rust backend");
#else
        Sink sink;
        SettingsManager settings;
        RustSdkMatrixClient client(&settings);
        watch(client, sink);
        listRoom(client, 0);
        QJsonObject done;
        done.insert(QStringLiteral("type"), QStringLiteral("initial_sync_done"));
        client.handleRustEventForTest(done);
        QVERIFY(client.initialSyncDone());

        listRoom(client, 1);
        QTRY_COMPARE_WITH_TIMEOUT(sink.highlightGaps.size(), 1, 5000);
        QCOMPARE(sink.highlightGaps.first().first, kRoom);
        QCOMPARE(sink.highlightGaps.first().second, 1);

        // A rise explained by a delivered highlighted event reports nothing.
        QJsonObject mention = message(QStringLiteral("$m"), QStringLiteral("@me"));
        mention.insert(QStringLiteral("push_notify"), true);
        mention.insert(QStringLiteral("push_highlight"), true);
        client.handleRustEventForTest(envelope(QStringLiteral("timeline_event"), mention));
        listRoom(client, 2);
        QTest::qWait(3000);
        QCOMPARE(sink.highlightGaps.size(), 1);
#endif
    }
};

QTEST_MAIN(NotificationProducerTest)
#include "NotificationProducerTest.moc"
