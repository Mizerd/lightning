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
    void parsesThreadSummary();
    void threadSummaryAbsentKeepsFallbackContract();
    void threadPanelIngestPreservesReplies();
    void parsesTypedStateActivity();
    // v0.7: typed media rows with type-correct reserved-geometry metadata.
    void parsesTypedMediaItems();
    // v0.7: MSC3381 polls.
    void parsesPollItem();
    void pollAbsentFieldsKeepDefaults();
    void pollVoteUpdatesInPlaceViaSet();

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

    // ── Phase 2: cold-cache thread loads empty then populates ───────
    void emptyThreadResetThenAppendPopulates();

    // ── Phase 5/7: thread-root summary live updates via set diffs ───
    void threadSummaryUpdatesInPlaceViaSet();
    void encryptedThreadSummaryDecryptsInPlace();
};

namespace {
QJsonObject threadRootItem(const QString &eventId, int replyCount,
                           const QString &kind, const QString &preview,
                           const QString &sender)
{
    QJsonObject item;
    item.insert(QStringLiteral("item_id"), QStringLiteral("uid-root"));
    item.insert(QStringLiteral("kind"), QStringLiteral("event"));
    item.insert(QStringLiteral("event_id"), eventId);
    item.insert(QStringLiteral("sender"), QStringLiteral("@alice:example.org"));
    item.insert(QStringLiteral("msgtype"), QStringLiteral("text"));
    item.insert(QStringLiteral("body"), QStringLiteral("root message"));
    item.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
    item.insert(QStringLiteral("is_thread_root"), true);
    item.insert(QStringLiteral("thread_reply_count"), replyCount);
    item.insert(QStringLiteral("thread_latest_kind"), kind);
    item.insert(QStringLiteral("thread_latest_preview"), preview);
    item.insert(QStringLiteral("thread_latest_sender"), sender);
    return item;
}
} // namespace

void RustTimelineIngestTest::parsesEventItem()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QStringLiteral("hello"));
    item.insert(QStringLiteral("sender_display_name"), QStringLiteral("Alice"));
    item.insert(QStringLiteral("sender_avatar_url"),
                QStringLiteral("mxc://example.org/alice"));
    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.itemId, QStringLiteral("uid1"));
    QCOMPARE(e.eventId, QStringLiteral("$ev1"));
    QCOMPARE(e.roomId, kRoom);
    QCOMPARE(e.body, QStringLiteral("hello"));
    QCOMPARE(e.senderDisplayName, QStringLiteral("Alice"));
    QCOMPARE(e.senderAvatarUrl, QStringLiteral("mxc://example.org/alice"));
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

void RustTimelineIngestTest::parsesTypedMediaItems()
{
    // Video: aspect dimensions + duration reserve the thumbnail geometry.
    QJsonObject video = itemJson(QStringLiteral("uidV"), QStringLiteral("$v"),
                                 QStringLiteral("clip.mp4"));
    video.insert(QStringLiteral("msgtype"), QStringLiteral("video"));
    video.insert(QStringLiteral("media_filename"), QStringLiteral("clip.mp4"));
    video.insert(QStringLiteral("media_width"), 1280);
    video.insert(QStringLiteral("media_height"), 720);
    video.insert(QStringLiteral("media_duration_ms"), 83000);
    video.insert(QStringLiteral("media_key"), QStringLiteral("$v"));
    video.insert(QStringLiteral("media_source_available"), true);
    video.insert(QStringLiteral("media_thumb_available"), true);
    const TimelineEvent v = eventFromItemJson(video, kRoom);
    QCOMPARE(v.type, TimelineEvent::Video);
    QCOMPARE(v.mediaWidth, 1280);
    QCOMPARE(v.mediaHeight, 720);
    QCOMPARE(v.mediaDurationMs, qint64(83000));
    QVERIFY(v.mediaThumbAvailable);

    // Audio: duration; plain audio is not a voice message.
    QJsonObject audio = itemJson(QStringLiteral("uidA"), QStringLiteral("$a"),
                                 QStringLiteral("song.ogg"));
    audio.insert(QStringLiteral("msgtype"), QStringLiteral("audio"));
    audio.insert(QStringLiteral("media_duration_ms"), 4000);
    const TimelineEvent a = eventFromItemJson(audio, kRoom);
    QCOMPARE(a.type, TimelineEvent::Audio);
    QCOMPARE(a.mediaDurationMs, qint64(4000));
    QVERIFY(!a.mediaIsVoice);

    // Voice: the MSC3245 marker flows through, and the real waveform
    // envelope arrives bounded and range-checked (out-of-range or
    // malformed entries are dropped, never clamped into fake data).
    QJsonObject voice = audio;
    voice.insert(QStringLiteral("media_voice"), true);
    voice.insert(QStringLiteral("media_waveform"),
                 QJsonArray{ 0, 50, 100, 101, -3, QStringLiteral("x") });
    const TimelineEvent vo = eventFromItemJson(voice, kRoom);
    QCOMPARE(vo.type, TimelineEvent::Audio);
    QVERIFY(vo.mediaIsVoice);
    QCOMPARE(vo.mediaWaveform, (QList<int>{ 0, 50, 100 }));

    // Sticker: image-shaped metadata with its own semantic type.
    QJsonObject sticker = itemJson(QStringLiteral("uidS"), QStringLiteral("$s"),
                                   QStringLiteral("party"));
    sticker.insert(QStringLiteral("msgtype"), QStringLiteral("sticker"));
    sticker.insert(QStringLiteral("media_width"), 512);
    sticker.insert(QStringLiteral("media_height"), 512);
    sticker.insert(QStringLiteral("media_key"), QStringLiteral("$s"));
    sticker.insert(QStringLiteral("media_source_available"), true);
    const TimelineEvent s = eventFromItemJson(sticker, kRoom);
    QCOMPARE(s.type, TimelineEvent::Sticker);
    QCOMPARE(s.mediaWidth, 512);
    QVERIFY(s.mediaSourceAvailable);
}

namespace {

QJsonObject pollItemJson(const QString &itemId, const QString &eventId)
{
    QJsonObject item = itemJson(itemId, eventId,
                                QStringLiteral("Favourite colour?"));
    item.insert(QStringLiteral("msgtype"), QStringLiteral("poll"));
    item.insert(QStringLiteral("poll_question"),
                QStringLiteral("Favourite colour?"));
    item.insert(QStringLiteral("poll_kind"), QStringLiteral("disclosed"));
    item.insert(QStringLiteral("poll_max_selections"), 1);
    item.insert(QStringLiteral("poll_total_voters"), 3);
    item.insert(QStringLiteral("poll_ended"), false);
    QJsonArray answers;
    QJsonObject a1;
    a1.insert(QStringLiteral("id"), QStringLiteral("a1"));
    a1.insert(QStringLiteral("text"), QStringLiteral("Blue"));
    a1.insert(QStringLiteral("count"), 2);
    a1.insert(QStringLiteral("by_me"), true);
    answers.append(a1);
    QJsonObject a2;
    a2.insert(QStringLiteral("id"), QStringLiteral("a2"));
    a2.insert(QStringLiteral("text"), QStringLiteral("Green"));
    a2.insert(QStringLiteral("count"), 1);
    a2.insert(QStringLiteral("by_me"), false);
    answers.append(a2);
    item.insert(QStringLiteral("poll_answers"), answers);
    return item;
}

} // namespace

void RustTimelineIngestTest::parsesPollItem()
{
    const TimelineEvent e = eventFromItemJson(pollItemJson(
        QStringLiteral("uidP"), QStringLiteral("$poll")), kRoom);
    QCOMPARE(e.type, TimelineEvent::Poll);
    QCOMPARE(e.pollQuestion, QStringLiteral("Favourite colour?"));
    QCOMPARE(e.pollKind, QStringLiteral("disclosed"));
    QCOMPARE(e.pollMaxSelections, 1);
    QCOMPARE(e.pollTotalVoters, 3);
    QVERIFY(!e.pollEnded);
    QCOMPARE(e.pollAnswers.size(), 2);
    QCOMPARE(e.pollAnswers.at(0).id, QStringLiteral("a1"));
    QCOMPARE(e.pollAnswers.at(0).text, QStringLiteral("Blue"));
    QCOMPARE(e.pollAnswers.at(0).count, 2);
    QVERIFY(e.pollAnswers.at(0).byMe);
    QCOMPARE(e.pollAnswers.at(1).count, 1);
    QVERIFY(!e.pollAnswers.at(1).byMe);
}

void RustTimelineIngestTest::pollAbsentFieldsKeepDefaults()
{
    // A poll payload with only the msgtype: defaults must be safe, and an
    // answer without a stable id is dropped rather than rendered.
    QJsonObject item = itemJson(QStringLiteral("uidP"), QStringLiteral("$p"),
                                QString());
    item.insert(QStringLiteral("msgtype"), QStringLiteral("poll"));
    QJsonArray answers;
    QJsonObject noId;
    noId.insert(QStringLiteral("text"), QStringLiteral("orphan"));
    answers.append(noId);
    item.insert(QStringLiteral("poll_answers"), answers);
    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.type, TimelineEvent::Poll);
    QCOMPARE(e.pollMaxSelections, 1);
    QCOMPARE(e.pollTotalVoters, 0);
    QVERIFY(!e.pollEnded);
    QVERIFY(e.pollAnswers.isEmpty());
}

void RustTimelineIngestTest::pollVoteUpdatesInPlaceViaSet()
{
    // A poll renders, then a vote arrives: the SDK aggregates into the SAME
    // timeline item and emits a Set diff — identity must be preserved.
    QJsonArray items;
    items.append(pollItemJson(QStringLiteral("uidP"), QStringLiteral("$poll")));
    auto mirror = eventsFromItemArray(items, kRoom);
    QCOMPARE(mirror.first().pollAnswers.at(1).count, 1);

    QJsonObject updated = pollItemJson(QStringLiteral("uidP"),
                                       QStringLiteral("$poll"));
    QJsonArray answers = updated.value(QStringLiteral("poll_answers")).toArray();
    QJsonObject a2 = answers.at(1).toObject();
    a2.insert(QStringLiteral("count"), 2);
    answers.replace(1, a2);
    updated.insert(QStringLiteral("poll_answers"), answers);
    updated.insert(QStringLiteral("poll_total_voters"), 4);

    QJsonObject diff = diffJson(QStringLiteral("set"));
    diff.insert(QStringLiteral("index"), 0);
    diff.insert(QStringLiteral("item"), updated);
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1); // in place — no duplicate row
    QCOMPARE(mirror.first().itemId, QStringLiteral("uidP"));
    QCOMPARE(mirror.first().pollAnswers.at(1).count, 2);
    QCOMPARE(mirror.first().pollTotalVoters, 4);

    // Poll end via the same mechanism.
    QJsonObject ended = updated;
    ended.insert(QStringLiteral("poll_ended"), true);
    QJsonObject endDiff = diffJson(QStringLiteral("set"));
    endDiff.insert(QStringLiteral("index"), 0);
    endDiff.insert(QStringLiteral("item"), ended);
    QCOMPARE(applyTimelineDiff(mirror, endDiff, kRoom).kind,
             DiffOutcome::Changed);
    QVERIFY(mirror.first().pollEnded);
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
    QCOMPARE(e.itemId, QStringLiteral("v2"));
    QVERIFY(e.eventId.isEmpty());

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

// v0.6.0: SDK thread-summary fields on a thread root item.
void RustTimelineIngestTest::parsesThreadSummary()
{
    QJsonObject item = itemJson(QStringLiteral("uid-root"),
                                QStringLiteral("$root"),
                                QStringLiteral("root message"));
    item.insert(QStringLiteral("is_thread_root"), true);
    item.insert(QStringLiteral("thread_reply_count"), 4);
    item.insert(QStringLiteral("thread_latest_preview"),
                QStringLiteral("latest reply"));
    item.insert(QStringLiteral("thread_latest_kind"), QStringLiteral("image"));
    item.insert(QStringLiteral("thread_latest_sender"),
                QStringLiteral("@carol:example.org"));
    item.insert(QStringLiteral("thread_latest_sender_display_name"),
                QStringLiteral("Carol"));
    item.insert(QStringLiteral("thread_latest_sender_avatar_url"),
                QStringLiteral("mxc://example.org/carol"));
    item.insert(QStringLiteral("thread_latest_timestamp_ms"), 1700000123000.0);
    item.insert(QStringLiteral("thread_unread"), true);

    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QVERIFY(e.isThreadRoot);
    QCOMPARE(e.threadReplyCount, 4);
    QCOMPARE(e.threadLatestPreview, QStringLiteral("latest reply"));
    QCOMPARE(e.threadLatestKind, QStringLiteral("image"));
    QCOMPARE(e.threadLatestSender, QStringLiteral("@carol:example.org"));
    QCOMPARE(e.threadLatestSenderDisplayName, QStringLiteral("Carol"));
    QCOMPARE(e.threadLatestSenderAvatarUrl,
             QStringLiteral("mxc://example.org/carol"));
    QCOMPARE(e.threadLatestTimestamp.toMSecsSinceEpoch(),
             Q_INT64_C(1700000123000));
    QVERIFY(e.threadUnread);
}

// Events without a summary keep the -1 "unknown" contract so non-SDK
// backends fall back to local reply counting, and a plain thread reply
// stays a reply — never a root.
void RustTimelineIngestTest::threadSummaryAbsentKeepsFallbackContract()
{
    QJsonObject reply = itemJson(QStringLiteral("uid-reply"),
                                 QStringLiteral("$reply"),
                                 QStringLiteral("in thread"));
    reply.insert(QStringLiteral("thread_root_id"), QStringLiteral("$root"));

    const TimelineEvent e = eventFromItemJson(reply, kRoom);
    QCOMPARE(e.threadRootId, QStringLiteral("$root"));
    QVERIFY(!e.isThreadRoot);
    QCOMPARE(e.threadReplyCount, -1);
    QVERIFY(e.threadLatestPreview.isEmpty());
    QVERIFY(!e.threadUnread);
    QVERIFY(!e.threadLatestTimestamp.isValid());
}

// Phase 1 split-responsibility guard. The main room timeline excludes
// m.thread replies at the SDK layer (TimelineFocus::Live hide_threaded_events);
// the identical ingest translation is reused for the thread panel, where those
// same replies MUST be preserved. This proves the C++ ingest is not a filter —
// it faithfully keeps every thread reply (with its thread_root_id) when the
// thread-focused SDK timeline delivers them, so hiding replies from main never
// means losing them.
void RustTimelineIngestTest::threadPanelIngestPreservesReplies()
{
    const QString root = QStringLiteral("$root");
    QJsonArray items;
    // Pinned root the panel shows above the replies.
    QJsonObject rootItem = itemJson(QStringLiteral("uid-root"), root,
                                    QStringLiteral("root message"));
    rootItem.insert(QStringLiteral("is_thread_root"), true);
    rootItem.insert(QStringLiteral("thread_reply_count"), 3);
    items.append(rootItem);
    // Three real m.thread replies, each carrying the authoritative relation.
    for (int i = 1; i <= 3; ++i) {
        QJsonObject reply =
            itemJson(QStringLiteral("uid-r%1").arg(i),
                     QStringLiteral("$reply%1").arg(i),
                     QStringLiteral("reply %1").arg(i));
        reply.insert(QStringLiteral("thread_root_id"), root);
        items.append(reply);
    }

    const auto mirror = eventsFromItemArray(items, kRoom);
    QCOMPARE(mirror.size(), 4);
    QVERIFY(mirror.first().isThreadRoot);
    QVERIFY(mirror.first().threadRootId.isEmpty()); // the root is not a reply
    int replies = 0;
    for (const auto &e : mirror) {
        if (e.threadRootId == root) {
            ++replies;
            QVERIFY(!e.isThreadRoot); // a reply is never classified as a root
        }
    }
    QCOMPARE(replies, 3);
}

void RustTimelineIngestTest::parsesTypedStateActivity()
{
    QJsonObject item = itemJson(QStringLiteral("state-1"),
                                QStringLiteral("$state"),
                                QStringLiteral("Alice changed the room topic."));
    item.insert(QStringLiteral("msgtype"), QStringLiteral("state"));
    item.insert(QStringLiteral("state_kind"), QStringLiteral("m.room.topic"));
    item.insert(QStringLiteral("state_target"), QStringLiteral("Project"));
    const TimelineEvent event = eventFromItemJson(item, kRoom);
    QCOMPARE(event.type, TimelineEvent::StateChange);
    QCOMPARE(event.stateKind, QStringLiteral("m.room.topic"));
    QCOMPARE(event.stateTarget, QStringLiteral("Project"));
    QCOMPARE(event.body, QStringLiteral("Alice changed the room topic."));
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
    QVERIFY(!tracker.readyForPagination(kRoom));
    // A reset for a different room (stale from the previous selection)
    // must not be adopted.
    QVERIFY(!tracker.adoptReset(QStringLiteral("!other:example.org"), 4));
    QVERIFY(!tracker.hasActiveTimeline());
    // The matching reset is adopted.
    QVERIFY(tracker.adoptReset(kRoom, 5));
    QVERIFY(tracker.hasActiveTimeline());
    QVERIFY(tracker.readyForPagination(kRoom));
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
    QVERIFY(!tracker.readyForPagination(kRoom));
    QVERIFY(!tracker.readyForPagination(QStringLiteral("!other:example.org")));
    QVERIFY(!tracker.accepts(kRoom, 3));
    QVERIFY(!tracker.adoptReset(kRoom, 4));
    QVERIFY(tracker.adoptReset(QStringLiteral("!other:example.org"), 4));
    QVERIFY(tracker.readyForPagination(QStringLiteral("!other:example.org")));
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

// Phase 2: a cold-cache thread subscribes with an empty snapshot (items=0)
// and the auto-pagination then delivers the replies as an append diff. This
// proves the generation the empty reset adopts stays valid for the follow-up
// diff, so the panel populates in place rather than staying permanently empty.
void RustTimelineIngestTest::emptyThreadResetThenAppendPopulates()
{
    const QString threadId = QStringLiteral("!room:example.org|$root");
    TimelineGenerationTracker tracker;
    tracker.request(threadId);
    // Empty initial snapshot — the exact items=0 subscription start.
    QVERIFY(tracker.adoptReset(threadId, 3));
    QList<TimelineEvent> mirror; // reset installed an empty mirror
    QVERIFY(mirror.isEmpty());

    // The auto-pagination's /relations fetch arrives as an append for the
    // same still-current generation and must be accepted.
    QVERIFY(tracker.accepts(threadId, 3));
    QJsonObject diff;
    diff.insert(QStringLiteral("type"), QStringLiteral("thread_diff"));
    diff.insert(QStringLiteral("op"), QStringLiteral("append"));
    QJsonArray items;
    for (int i = 1; i <= 3; ++i) {
        QJsonObject reply =
            itemJson(QStringLiteral("uid-r%1").arg(i),
                     QStringLiteral("$reply%1").arg(i),
                     QStringLiteral("reply %1").arg(i));
        reply.insert(QStringLiteral("thread_root_id"), QStringLiteral("$root"));
        items.append(reply);
    }
    diff.insert(QStringLiteral("items"), items);
    const auto outcome = applyTimelineDiff(mirror, diff, threadId);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QCOMPARE(mirror.size(), 3);

    // A stale-generation append (an older open of the same thread) is rejected
    // — it can never repopulate a superseded panel.
    QVERIFY(!tracker.accepts(threadId, 2));
}

// Phase 5: a thread root's summary is carried on the root event item, so any
// thread activity (new reply, edit of the latest, redaction, receipt) arrives
// as a `set` diff on the root. This proves the diff updates the summary IN
// PLACE — the same row, new count/preview/kind — which is what drives the
// bound ThreadSummaryCard roles to refresh incrementally (no rescan, no extra
// row). An idempotent duplicate set is a no-op change on the same row.
void RustTimelineIngestTest::threadSummaryUpdatesInPlaceViaSet()
{
    const QString root = QStringLiteral("$root");
    QJsonArray items;
    items.append(threadRootItem(root, 1, QStringLiteral("text"),
                                QStringLiteral("first reply"),
                                QStringLiteral("@bob:example.org")));
    auto mirror = eventsFromItemArray(items, kRoom);
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().threadReplyCount, 1);
    QCOMPARE(mirror.first().threadLatestPreview, QStringLiteral("first reply"));

    // A newer reply: count rises, latest preview/sender change.
    QJsonObject set = diffJson(QStringLiteral("set"));
    set.insert(QStringLiteral("index"), 0);
    set.insert(QStringLiteral("item"),
               threadRootItem(root, 2, QStringLiteral("image"),
                              QStringLiteral("photo.png"),
                              QStringLiteral("@carol:example.org")));
    auto outcome = applyTimelineDiff(mirror, set, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1);   // in place — no duplicate main-timeline row
    QVERIFY(mirror.first().isThreadRoot);
    QCOMPARE(mirror.first().threadReplyCount, 2);
    QCOMPARE(mirror.first().threadLatestKind, QStringLiteral("image"));
    QCOMPARE(mirror.first().threadLatestSender,
             QStringLiteral("@carol:example.org"));

    // Redaction of the latest reply: kind falls back safely, row unchanged.
    QJsonObject redact = diffJson(QStringLiteral("set"));
    redact.insert(QStringLiteral("index"), 0);
    redact.insert(QStringLiteral("item"),
                  threadRootItem(root, 2, QStringLiteral("redacted"),
                                 QString(), QStringLiteral("@carol:example.org")));
    outcome = applyTimelineDiff(mirror, redact, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().threadLatestKind, QStringLiteral("redacted"));
}

// Phase 7: an encrypted latest reply surfaces as kind "encrypted" (the card
// shows a safe placeholder). When the key arrives the SDK re-sends the root
// summary as a `set` with the decrypted preview — the summary updates in the
// same row (no duplicate, no flashed main-timeline reply), and a stale
// generation can never apply the decrypted update to a superseded panel.
void RustTimelineIngestTest::encryptedThreadSummaryDecryptsInPlace()
{
    const QString root = QStringLiteral("$root");
    QJsonArray items;
    items.append(threadRootItem(root, 3, QStringLiteral("encrypted"),
                                QString(), QStringLiteral("@dave:example.org")));
    auto mirror = eventsFromItemArray(items, kRoom);
    QCOMPARE(mirror.first().threadLatestKind, QStringLiteral("encrypted"));

    QJsonObject decrypted = diffJson(QStringLiteral("set"));
    decrypted.insert(QStringLiteral("index"), 0);
    decrypted.insert(QStringLiteral("item"),
                     threadRootItem(root, 3, QStringLiteral("text"),
                                    QStringLiteral("now readable"),
                                    QStringLiteral("@dave:example.org")));
    const auto outcome = applyTimelineDiff(mirror, decrypted, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1);   // same row — no duplicate
    QCOMPARE(mirror.first().threadLatestKind, QStringLiteral("text"));
    QCOMPARE(mirror.first().threadLatestPreview, QStringLiteral("now readable"));

    // Generation isolation: a stale-generation set for the same thread id is
    // rejected by the tracker before it can rewrite a newer account's summary.
    TimelineGenerationTracker tracker;
    tracker.request(kRoom);
    QVERIFY(tracker.adoptReset(kRoom, 5));
    QVERIFY(!tracker.accepts(kRoom, 4)); // stale decryption completion
    QVERIFY(tracker.accepts(kRoom, 5));
}

QTEST_GUILESS_MAIN(RustTimelineIngestTest)
#include "RustTimelineIngestTest.moc"
