// v0.5.7: live-timeline diff translation and generation tracking.
//
// Exercises matrix::rust_timeline without cargo, a Rust handle, or a
// homeserver: every VectorDiff envelope the Rust bridge can emit, index
// validation, malformed-diff rejection, undecryptable → decrypted
// replacement payloads, and stale room/lifecycle generation rejection.

#include "matrix/RustTimelineIngest.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest/QtTest>

using matrix::rust_timeline::DiffOutcome;
using matrix::rust_timeline::TimelineGenerationTracker;
using matrix::rust_timeline::applyTimelineDiff;
using matrix::rust_timeline::eventFromItemJson;
using matrix::rust_timeline::eventsFromItemArray;

namespace {

const QString kRoom = QStringLiteral("!room:example.org");

QJsonObject itemJson(const QString &itemId, const QString &eventId,
                     const QString &body)
{
    QJsonObject item;
    item.insert(QStringLiteral("item_id"), itemId);
    item.insert(QStringLiteral("kind"), QStringLiteral("event"));
    item.insert(QStringLiteral("event_id"), eventId);
    item.insert(QStringLiteral("sender"), QStringLiteral("@alice:example.org"));
    item.insert(QStringLiteral("msgtype"), QStringLiteral("text"));
    item.insert(QStringLiteral("body"), body);
    item.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
    return item;
}

QJsonObject diffJson(const QString &op)
{
    QJsonObject diff;
    diff.insert(QStringLiteral("type"), QStringLiteral("timeline_diff"));
    diff.insert(QStringLiteral("room_id"), kRoom);
    diff.insert(QStringLiteral("op"), op);
    return diff;
}

QList<TimelineEvent> mirrorOf(int count)
{
    QJsonArray items;
    for (int i = 0; i < count; ++i) {
        items.append(itemJson(QStringLiteral("item%1").arg(i),
                              QStringLiteral("$ev%1").arg(i),
                              QStringLiteral("body %1").arg(i)));
    }
    return eventsFromItemArray(items, kRoom);
}

} // namespace

class RustTimelineIngestTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ── Item conversion ─────────────────────────────────────────────
    void parsesEventItem();
    void parsesUndecryptableItem();
    void parsesLocalEchoStates();
    void parsesVirtualItems();
    void parsesReactionsAndReply();

    // ── Diff application: every VectorDiff variant ──────────────────
    void appendAppends();
    void emptyAppendIsNoOp();
    void pushBackAppendsOne();
    void pushFrontPrepends();
    void insertAtIndex();
    void insertAtEndIsValid();
    void setReplacesInPlace();
    void removeAtIndex();
    void popFrontRemovesFirst();
    void popBackRemovesLast();
    void clearEmpties();
    void truncateShortens();
    void resetReplacesAll();

    // ── Validation ──────────────────────────────────────────────────
    void insertBeyondEndRejected();
    void setOutOfRangeRejected();
    void removeOutOfRangeRejected();
    void popFrontOnEmptyRejected();
    void truncateBeyondSizeRejected();
    void unknownOpRejected();
    void missingItemRejected();

    // ── Undecryptable → decrypted in place ──────────────────────────
    void undecryptableBecomesDecryptedViaSet();

    // ── Local echo reconciliation via Set ───────────────────────────
    void localEchoReconciledViaSet();

    // ── Generation tracking ─────────────────────────────────────────
    void trackerAdoptsRequestedRoomOnly();
    void trackerRejectsStaleGenerations();
    void trackerRejectsAfterNewRequest();
    void trackerResetClearsEverything();
};

void RustTimelineIngestTest::parsesEventItem()
{
    const TimelineEvent e = eventFromItemJson(
        itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                 QStringLiteral("hello")),
        kRoom);
    QCOMPARE(e.itemId, QStringLiteral("uid1"));
    QCOMPARE(e.eventId, QStringLiteral("$ev1"));
    QCOMPARE(e.roomId, kRoom);
    QCOMPARE(e.body, QStringLiteral("hello"));
    QCOMPARE(e.type, TimelineEvent::TextMessage);
    QCOMPARE(e.status, TimelineEvent::Sent);
    QVERIFY(!e.isVirtual());
    QVERIFY(!e.isLocalEcho);
}

void RustTimelineIngestTest::parsesUndecryptableItem()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QString());
    item.insert(QStringLiteral("msgtype"), QStringLiteral("encrypted"));
    item.insert(QStringLiteral("is_encrypted"), true);
    item.insert(QStringLiteral("is_decrypted"), false);
    item.insert(QStringLiteral("undecryptable"), true);
    item.insert(QStringLiteral("error_kind"), QStringLiteral("no_key"));
    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QVERIFY(e.undecryptable);
    QVERIFY(e.isEncrypted);
    QVERIFY(!e.isDecrypted);
    QCOMPARE(e.errorKind, QStringLiteral("no_key"));
    // Honest placeholder, never an empty bubble, never ciphertext.
    QVERIFY(!e.body.isEmpty());
}

void RustTimelineIngestTest::parsesLocalEchoStates()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QString(),
                                QStringLiteral("outgoing"));
    item.insert(QStringLiteral("transaction_id"), QStringLiteral("txn1"));
    item.insert(QStringLiteral("is_local_echo"), true);
    item.insert(QStringLiteral("send_state"), QStringLiteral("sending"));
    TimelineEvent e = eventFromItemJson(item, kRoom);
    QVERIFY(e.isLocalEcho);
    QCOMPARE(e.transactionId, QStringLiteral("txn1"));
    QCOMPARE(e.status, TimelineEvent::Sending);

    item.insert(QStringLiteral("send_state"), QStringLiteral("failed"));
    item.insert(QStringLiteral("send_error"), QStringLiteral("network"));
    e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.status, TimelineEvent::Failed);
    QCOMPARE(e.sendErrorCategory, QStringLiteral("network"));

    item.insert(QStringLiteral("send_state"), QStringLiteral("sent"));
    e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.status, TimelineEvent::Sent);
}

void RustTimelineIngestTest::parsesVirtualItems()
{
    QJsonObject divider;
    divider.insert(QStringLiteral("item_id"), QStringLiteral("v1"));
    divider.insert(QStringLiteral("kind"), QStringLiteral("date_divider"));
    divider.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
    TimelineEvent e = eventFromItemJson(divider, kRoom);
    QCOMPARE(e.type, TimelineEvent::DateDivider);
    QVERIFY(e.isVirtual());
    QVERIFY(e.eventId.isEmpty());

    QJsonObject marker;
    marker.insert(QStringLiteral("item_id"), QStringLiteral("v2"));
    marker.insert(QStringLiteral("kind"), QStringLiteral("read_marker"));
    e = eventFromItemJson(marker, kRoom);
    QCOMPARE(e.type, TimelineEvent::ReadMarker);

    QJsonObject start;
    start.insert(QStringLiteral("item_id"), QStringLiteral("v3"));
    start.insert(QStringLiteral("kind"), QStringLiteral("timeline_start"));
    e = eventFromItemJson(start, kRoom);
    QCOMPARE(e.type, TimelineEvent::TimelineStart);
}

void RustTimelineIngestTest::parsesReactionsAndReply()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QStringLiteral("hello"));
    QJsonArray reactions;
    QJsonObject up;
    up.insert(QStringLiteral("key"), QStringLiteral("👍"));
    up.insert(QStringLiteral("count"), 2);
    up.insert(QStringLiteral("by_me"), true);
    reactions.append(up);
    item.insert(QStringLiteral("reactions"), reactions);
    item.insert(QStringLiteral("reply_to_event_id"), QStringLiteral("$orig"));
    item.insert(QStringLiteral("reply_to_sender"),
                QStringLiteral("@bob:example.org"));
    item.insert(QStringLiteral("reply_to_preview"), QStringLiteral("original"));

    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.reactions.size(), 1);
    QCOMPARE(e.reactions.first().key, QStringLiteral("👍"));
    QCOMPARE(e.reactions.first().count, 2);
    QVERIFY(e.reactions.first().byMe);
    QCOMPARE(e.replyToEventId, QStringLiteral("$orig"));
    QCOMPARE(e.replyToSender, QStringLiteral("@bob:example.org"));
    QCOMPARE(e.replyToPreview, QStringLiteral("original"));
}

void RustTimelineIngestTest::appendAppends()
{
    auto mirror = mirrorOf(2);
    QJsonObject diff = diffJson(QStringLiteral("append"));
    QJsonArray items;
    items.append(itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                          QStringLiteral("A")));
    items.append(itemJson(QStringLiteral("b"), QStringLiteral("$b"),
                          QStringLiteral("B")));
    diff.insert(QStringLiteral("items"), items);
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QCOMPARE(outcome.items.size(), 2);
    QCOMPARE(mirror.size(), 4);
    QCOMPARE(mirror.at(3).eventId, QStringLiteral("$b"));
}

void RustTimelineIngestTest::emptyAppendIsNoOp()
{
    auto mirror = mirrorOf(2);
    QJsonObject diff = diffJson(QStringLiteral("append"));
    diff.insert(QStringLiteral("items"), QJsonArray());
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QVERIFY(outcome.items.isEmpty());
    QCOMPARE(mirror.size(), 2);
}

void RustTimelineIngestTest::pushBackAppendsOne()
{
    auto mirror = mirrorOf(1);
    QJsonObject diff = diffJson(QStringLiteral("push_back"));
    diff.insert(QStringLiteral("item"),
                itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                         QStringLiteral("A")));
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QCOMPARE(mirror.size(), 2);
    QCOMPARE(mirror.last().eventId, QStringLiteral("$a"));
}

void RustTimelineIngestTest::pushFrontPrepends()
{
    auto mirror = mirrorOf(1);
    QJsonObject diff = diffJson(QStringLiteral("push_front"));
    diff.insert(QStringLiteral("item"),
                itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                         QStringLiteral("A")));
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Prepended);
    QCOMPARE(mirror.size(), 2);
    QCOMPARE(mirror.first().eventId, QStringLiteral("$a"));
}

void RustTimelineIngestTest::insertAtIndex()
{
    auto mirror = mirrorOf(3);
    QJsonObject diff = diffJson(QStringLiteral("insert"));
    diff.insert(QStringLiteral("index"), 1);
    diff.insert(QStringLiteral("item"),
                itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                         QStringLiteral("A")));
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Inserted);
    QCOMPARE(outcome.index, 1);
    QCOMPARE(mirror.size(), 4);
    QCOMPARE(mirror.at(1).eventId, QStringLiteral("$a"));
}

void RustTimelineIngestTest::insertAtEndIsValid()
{
    auto mirror = mirrorOf(2);
    QJsonObject diff = diffJson(QStringLiteral("insert"));
    diff.insert(QStringLiteral("index"), 2);
    diff.insert(QStringLiteral("item"),
                itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                         QStringLiteral("A")));
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Inserted);
    QCOMPARE(mirror.size(), 3);
}

void RustTimelineIngestTest::setReplacesInPlace()
{
    auto mirror = mirrorOf(3);
    QJsonObject diff = diffJson(QStringLiteral("set"));
    diff.insert(QStringLiteral("index"), 2);
    diff.insert(QStringLiteral("item"),
                itemJson(QStringLiteral("item2"), QStringLiteral("$ev2"),
                         QStringLiteral("edited body")));
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(outcome.index, 2);
    QCOMPARE(mirror.size(), 3);
    QCOMPARE(mirror.at(2).body, QStringLiteral("edited body"));
}

void RustTimelineIngestTest::removeAtIndex()
{
    auto mirror = mirrorOf(3);
    QJsonObject diff = diffJson(QStringLiteral("remove"));
    diff.insert(QStringLiteral("index"), 1);
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Removed);
    QCOMPARE(outcome.index, 1);
    QCOMPARE(mirror.size(), 2);
    QCOMPARE(mirror.at(1).eventId, QStringLiteral("$ev2"));
}

void RustTimelineIngestTest::popFrontRemovesFirst()
{
    auto mirror = mirrorOf(2);
    const auto outcome =
        applyTimelineDiff(mirror, diffJson(QStringLiteral("pop_front")), kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Removed);
    QCOMPARE(outcome.index, 0);
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().eventId, QStringLiteral("$ev1"));
}

void RustTimelineIngestTest::popBackRemovesLast()
{
    auto mirror = mirrorOf(2);
    const auto outcome =
        applyTimelineDiff(mirror, diffJson(QStringLiteral("pop_back")), kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Removed);
    QCOMPARE(outcome.index, 1);
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().eventId, QStringLiteral("$ev0"));
}

void RustTimelineIngestTest::clearEmpties()
{
    auto mirror = mirrorOf(3);
    const auto outcome =
        applyTimelineDiff(mirror, diffJson(QStringLiteral("clear")), kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Cleared);
    QVERIFY(mirror.isEmpty());
}

void RustTimelineIngestTest::truncateShortens()
{
    auto mirror = mirrorOf(4);
    QJsonObject diff = diffJson(QStringLiteral("truncate"));
    diff.insert(QStringLiteral("length"), 2);
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Truncated);
    QCOMPARE(outcome.length, 2);
    QCOMPARE(mirror.size(), 2);
    QCOMPARE(mirror.last().eventId, QStringLiteral("$ev1"));
}

void RustTimelineIngestTest::resetReplacesAll()
{
    auto mirror = mirrorOf(3);
    QJsonObject diff = diffJson(QStringLiteral("reset"));
    QJsonArray items;
    items.append(itemJson(QStringLiteral("n"), QStringLiteral("$new"),
                          QStringLiteral("fresh")));
    diff.insert(QStringLiteral("items"), items);
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Reset);
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().eventId, QStringLiteral("$new"));
}

void RustTimelineIngestTest::insertBeyondEndRejected()
{
    auto mirror = mirrorOf(2);
    QJsonObject diff = diffJson(QStringLiteral("insert"));
    diff.insert(QStringLiteral("index"), 3);
    diff.insert(QStringLiteral("item"),
                itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                         QStringLiteral("A")));
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Invalid);
    QCOMPARE(mirror.size(), 2); // untouched
}

void RustTimelineIngestTest::setOutOfRangeRejected()
{
    auto mirror = mirrorOf(2);
    QJsonObject diff = diffJson(QStringLiteral("set"));
    diff.insert(QStringLiteral("index"), 2);
    diff.insert(QStringLiteral("item"),
                itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                         QStringLiteral("A")));
    QCOMPARE(applyTimelineDiff(mirror, diff, kRoom).kind, DiffOutcome::Invalid);

    diff.insert(QStringLiteral("index"), -1);
    QCOMPARE(applyTimelineDiff(mirror, diff, kRoom).kind, DiffOutcome::Invalid);
    QCOMPARE(mirror.size(), 2);
}

void RustTimelineIngestTest::removeOutOfRangeRejected()
{
    auto mirror = mirrorOf(2);
    QJsonObject diff = diffJson(QStringLiteral("remove"));
    diff.insert(QStringLiteral("index"), 5);
    QCOMPARE(applyTimelineDiff(mirror, diff, kRoom).kind, DiffOutcome::Invalid);
    QCOMPARE(mirror.size(), 2);
}

void RustTimelineIngestTest::popFrontOnEmptyRejected()
{
    QList<TimelineEvent> mirror;
    QCOMPARE(applyTimelineDiff(mirror, diffJson(QStringLiteral("pop_front")),
                               kRoom)
                 .kind,
             DiffOutcome::Invalid);
    QCOMPARE(applyTimelineDiff(mirror, diffJson(QStringLiteral("pop_back")),
                               kRoom)
                 .kind,
             DiffOutcome::Invalid);
}

void RustTimelineIngestTest::truncateBeyondSizeRejected()
{
    auto mirror = mirrorOf(2);
    QJsonObject diff = diffJson(QStringLiteral("truncate"));
    diff.insert(QStringLiteral("length"), 5);
    QCOMPARE(applyTimelineDiff(mirror, diff, kRoom).kind, DiffOutcome::Invalid);
    QCOMPARE(mirror.size(), 2);
}

void RustTimelineIngestTest::unknownOpRejected()
{
    auto mirror = mirrorOf(1);
    QCOMPARE(applyTimelineDiff(mirror, diffJson(QStringLiteral("explode")),
                               kRoom)
                 .kind,
             DiffOutcome::Invalid);
    QCOMPARE(applyTimelineDiff(mirror, QJsonObject(), kRoom).kind,
             DiffOutcome::Invalid);
    QCOMPARE(mirror.size(), 1);
}

void RustTimelineIngestTest::missingItemRejected()
{
    auto mirror = mirrorOf(1);
    // insert/set/push without an item payload are malformed.
    QJsonObject insert = diffJson(QStringLiteral("insert"));
    insert.insert(QStringLiteral("index"), 0);
    QCOMPARE(applyTimelineDiff(mirror, insert, kRoom).kind, DiffOutcome::Invalid);

    QJsonObject set = diffJson(QStringLiteral("set"));
    set.insert(QStringLiteral("index"), 0);
    QCOMPARE(applyTimelineDiff(mirror, set, kRoom).kind, DiffOutcome::Invalid);

    QCOMPARE(applyTimelineDiff(mirror, diffJson(QStringLiteral("push_back")),
                               kRoom)
                 .kind,
             DiffOutcome::Invalid);
    QCOMPARE(mirror.size(), 1);
}

void RustTimelineIngestTest::undecryptableBecomesDecryptedViaSet()
{
    // Start with one undecryptable row (as after opening an encrypted room
    // without keys)…
    QJsonObject utd = itemJson(QStringLiteral("uid1"), QStringLiteral("$enc"),
                               QString());
    utd.insert(QStringLiteral("msgtype"), QStringLiteral("encrypted"));
    utd.insert(QStringLiteral("undecryptable"), true);
    utd.insert(QStringLiteral("is_encrypted"), true);
    utd.insert(QStringLiteral("is_decrypted"), false);
    QJsonArray items;
    items.append(utd);
    auto mirror = eventsFromItemArray(items, kRoom);
    QVERIFY(mirror.first().undecryptable);

    // …then the SDK emits a Set for the same index after key import.
    QJsonObject decrypted = itemJson(QStringLiteral("uid1"),
                                     QStringLiteral("$enc"),
                                     QStringLiteral("now readable"));
    decrypted.insert(QStringLiteral("is_encrypted"), true);
    decrypted.insert(QStringLiteral("is_decrypted"), true);
    QJsonObject diff = diffJson(QStringLiteral("set"));
    diff.insert(QStringLiteral("index"), 0);
    diff.insert(QStringLiteral("item"), decrypted);

    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1); // in place — no duplicate row
    QVERIFY(!mirror.first().undecryptable);
    QVERIFY(mirror.first().isDecrypted);
    QCOMPARE(mirror.first().body, QStringLiteral("now readable"));
    QCOMPARE(mirror.first().eventId, QStringLiteral("$enc"));
    QCOMPARE(mirror.first().itemId, QStringLiteral("uid1"));
}

void RustTimelineIngestTest::localEchoReconciledViaSet()
{
    // Local echo appears via push_back…
    QJsonObject echo = itemJson(QStringLiteral("uid-local"), QString(),
                                QStringLiteral("outgoing"));
    echo.insert(QStringLiteral("transaction_id"), QStringLiteral("txn1"));
    echo.insert(QStringLiteral("is_local_echo"), true);
    echo.insert(QStringLiteral("send_state"), QStringLiteral("sending"));
    QList<TimelineEvent> mirror;
    QJsonObject push = diffJson(QStringLiteral("push_back"));
    push.insert(QStringLiteral("item"), echo);
    QCOMPARE(applyTimelineDiff(mirror, push, kRoom).kind, DiffOutcome::Appended);
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().status, TimelineEvent::Sending);

    // …and the remote echo reconciles it in place — same row count.
    QJsonObject remote = itemJson(QStringLiteral("uid-local"),
                                  QStringLiteral("$server"),
                                  QStringLiteral("outgoing"));
    remote.insert(QStringLiteral("send_state"), QStringLiteral("sent"));
    QJsonObject set = diffJson(QStringLiteral("set"));
    set.insert(QStringLiteral("index"), 0);
    set.insert(QStringLiteral("item"), remote);
    QCOMPARE(applyTimelineDiff(mirror, set, kRoom).kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1); // no duplicate
    QCOMPARE(mirror.first().eventId, QStringLiteral("$server"));
    QCOMPARE(mirror.first().status, TimelineEvent::Sent);
}

void RustTimelineIngestTest::trackerAdoptsRequestedRoomOnly()
{
    TimelineGenerationTracker tracker;
    tracker.request(kRoom);
    // A reset for a different room (stale from the previous selection)
    // must not be adopted.
    QVERIFY(!tracker.adoptReset(QStringLiteral("!other:example.org"), 4));
    QVERIFY(!tracker.hasActiveTimeline());
    // The matching reset is adopted.
    QVERIFY(tracker.adoptReset(kRoom, 5));
    QVERIFY(tracker.hasActiveTimeline());
    QCOMPARE(tracker.generation(), quint64(5));
    QVERIFY(tracker.accepts(kRoom, 5));
}

void RustTimelineIngestTest::trackerRejectsStaleGenerations()
{
    TimelineGenerationTracker tracker;
    tracker.request(kRoom);
    QVERIFY(tracker.adoptReset(kRoom, 7));
    // Diffs from an older open of the same room are rejected.
    QVERIFY(!tracker.accepts(kRoom, 6));
    // Diffs from another room are rejected.
    QVERIFY(!tracker.accepts(QStringLiteral("!other:example.org"), 7));
    // A zero generation (never valid) is rejected.
    QVERIFY(!tracker.adoptReset(kRoom, 0));
    // Re-adopting an older reset is rejected.
    QVERIFY(!tracker.adoptReset(kRoom, 7));
    QVERIFY(tracker.adoptReset(kRoom, 8));
    QVERIFY(!tracker.accepts(kRoom, 7));
    QVERIFY(tracker.accepts(kRoom, 8));
}

void RustTimelineIngestTest::trackerRejectsAfterNewRequest()
{
    TimelineGenerationTracker tracker;
    tracker.request(kRoom);
    QVERIFY(tracker.adoptReset(kRoom, 3));
    // Switching rooms invalidates the previous adoption immediately: no
    // diff for the old room may land in the new one.
    tracker.request(QStringLiteral("!other:example.org"));
    QVERIFY(!tracker.accepts(kRoom, 3));
    QVERIFY(!tracker.adoptReset(kRoom, 4));
    QVERIFY(tracker.adoptReset(QStringLiteral("!other:example.org"), 4));
    QVERIFY(tracker.accepts(QStringLiteral("!other:example.org"), 4));
}

void RustTimelineIngestTest::trackerResetClearsEverything()
{
    TimelineGenerationTracker tracker;
    tracker.request(kRoom);
    QVERIFY(tracker.adoptReset(kRoom, 2));
    tracker.reset(); // sign-out
    QVERIFY(!tracker.hasActiveTimeline());
    QVERIFY(!tracker.accepts(kRoom, 2));
    QVERIFY(!tracker.adoptReset(kRoom, 3)); // nothing requested anymore
}

QTEST_GUILESS_MAIN(RustTimelineIngestTest)
#include "RustTimelineIngestTest.moc"
