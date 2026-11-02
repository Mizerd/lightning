
#include "matrix/NotificationProducerState.h"
#include "matrix/RustTimelineIngest.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest/QtTest>

using matrix::rust_timeline::DiffOutcome;
using matrix::rust_timeline::TimelineGenerationTracker;
using matrix::rust_timeline::applyTimelineDiff;
using matrix::rust_timeline::eventFromItemJson;
using matrix::rust_timeline::eventsFromItemArray;
using matrix::rust_timeline::readPushVerdict;

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

}

class RustTimelineIngestTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesEventItem();
    void aWhitespaceOnlyDisplayNameIsNoName();
    void parsesFormattedBody();
    void readsThePushVerdictOnlyFromARealBoolean();
    void aLiveAppendCarriesItsPushVerdict();
    void aResyncAppendCarriesTheBacklogMark();
    void parsesUndecryptableItem();
    void parsesLocalEchoStates();
    void parsesMediaUploadProgress();
    void parsesVirtualItems();
    void parsesReactionsAndReply();
    void parsesReactionSenders();
    void reactionSendersAbsentLeavesEmptyList();
    void parsesReadReceipts();
    void readReceiptsMoveBetweenRowsViaSet();
    void parsesThreadSummary();
    void threadSummaryAbsentKeepsFallbackContract();
    void threadPanelIngestPreservesReplies();
    void parsesTypedStateActivity();
    void parsesTypedProfileChange();
    void profileChangeAbsentFieldsKeepDefaults();
    void profileNameIsBoundedWithoutSplittingSurrogatePairs();
    void parsesTypedMediaItems();
    void parsesGalleryItemsBoundedAndClosed();
    void parsesReplyTargetKindAndCount();
    void mediaFilenameLosesBidiControls();
    void parsesPollItem();
    void pollAbsentFieldsKeepDefaults();
    void pollVoteUpdatesInPlaceViaSet();

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

    void insertBeyondEndRejected();
    void setOutOfRangeRejected();
    void removeOutOfRangeRejected();
    void popFrontOnEmptyRejected();
    void truncateBeyondSizeRejected();
    void unknownOpRejected();
    void missingItemRejected();

    void undecryptableBecomesDecryptedViaSet();

    void localEchoReconciledViaSet();

    void trackerAdoptsRequestedRoomOnly();
    void trackerRejectsStaleGenerations();
    void trackerRejectsAfterNewRequest();
    void trackerResetClearsEverything();

    void emptyThreadResetThenAppendPopulates();

    void threadSummaryUpdatesInPlaceViaSet();
    void encryptedThreadSummaryDecryptsInPlace();

    void membersFromPayloadBuildsCacheEntries();
    void aSnapshotDropsALocalEchoTheRemoteEventAlreadyCovers();

    void readsThePushSoundTweakOnlyWithAVerdict();
    void aSyncPayloadCarriesBacklogReplyAndEditFacts();
    void anUndecryptableSyncPayloadKeepsItsThreadRoot();
    void aSyncPollStartIsAPollRow();
    void theOpenRoomsThreadReplyIsObservedNeverAppended();
    void aRemoteInsertAboveAPendingSendIsALiveArrival();
    void aPaginationInsertIsNeverALiveArrival();
    void aSetReportsDecryptionAndRedactionOfTheSameEventOnly();
    void theUndecryptableSightingsAreBoundedAndTakenOnce();
    void aHighlightRiseNobodyWasToldAboutIsReported();
    void aHighlightRiseExplainedByADeliveredEventIsNot();
    void aHighlightRiseIsNeverReportedForHistoryOrAFirstCount();
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
}

void RustTimelineIngestTest::aWhitespaceOnlyDisplayNameIsNoName()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QStringLiteral("hello"));
    item.insert(QStringLiteral("sender_display_name"),
                QStringLiteral(" \t  "));
    QVERIFY(eventFromItemJson(item, kRoom).senderDisplayName.isEmpty());
    item.insert(QStringLiteral("sender_display_name"),
                QStringLiteral("  Alice "));
    QCOMPARE(eventFromItemJson(item, kRoom).senderDisplayName,
             QStringLiteral("Alice"));
}

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

void RustTimelineIngestTest::parsesFormattedBody()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QStringLiteral("[@t](https://matrix.to/#/%40t%3Ax)"));
    item.insert(QStringLiteral("formatted_body"),
                QStringLiteral("<a href=\"https://matrix.to/#/%40t%3Ax\">@t</a>"));
    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.formattedBody,
             QStringLiteral("<a href=\"https://matrix.to/#/%40t%3Ax\">@t</a>"));

    const TimelineEvent plain = eventFromItemJson(
        itemJson(QStringLiteral("uid2"), QStringLiteral("$ev2"),
                 QStringLiteral("plain")), kRoom);
    QVERIFY(plain.formattedBody.isEmpty());
}

void RustTimelineIngestTest::readsThePushVerdictOnlyFromARealBoolean()
{
    using Verdict = TimelineEvent::PushVerdict;
    const auto read = [](const QJsonObject &payload) {
        TimelineEvent e;
        readPushVerdict(payload, e);
        return e;
    };

    const TimelineEvent absent = read(QJsonObject{});
    QCOMPARE(absent.pushVerdict, Verdict::Unknown);
    QVERIFY(!absent.pushHighlight);

    const TimelineEvent quiet = read(QJsonObject{
        {QStringLiteral("push_notify"), false},
        {QStringLiteral("push_highlight"), false}});
    QCOMPARE(quiet.pushVerdict, Verdict::Quiet);
    QVERIFY(!quiet.pushHighlight);

    const TimelineEvent loud = read(QJsonObject{
        {QStringLiteral("push_notify"), true},
        {QStringLiteral("push_highlight"), true}});
    QCOMPARE(loud.pushVerdict, Verdict::Notify);
    QVERIFY(loud.pushHighlight);

    const TimelineEvent noHighlight =
        read(QJsonObject{{QStringLiteral("push_notify"), true}});
    QCOMPARE(noHighlight.pushVerdict, Verdict::Notify);
    QVERIFY(!noHighlight.pushHighlight);

    const TimelineEvent stringy = read(QJsonObject{
        {QStringLiteral("push_notify"), QStringLiteral("false")},
        {QStringLiteral("push_highlight"), true}});
    QCOMPARE(stringy.pushVerdict, Verdict::Unknown);
    QVERIFY(!stringy.pushHighlight);
    const TimelineEvent numeric =
        read(QJsonObject{{QStringLiteral("push_notify"), 0}});
    QCOMPARE(numeric.pushVerdict, Verdict::Unknown);
}

void RustTimelineIngestTest::aLiveAppendCarriesItsPushVerdict()
{
    auto mirror = mirrorOf(1);
    QJsonObject item = itemJson(QStringLiteral("a"), QStringLiteral("$a"),
                                QStringLiteral("plain"));
    item.insert(QStringLiteral("push_notify"), false);
    item.insert(QStringLiteral("push_highlight"), false);
    QJsonObject pushBack = diffJson(QStringLiteral("push_back"));
    pushBack.insert(QStringLiteral("item"), item);
    auto outcome = applyTimelineDiff(mirror, pushBack, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QCOMPARE(outcome.items.size(), 1);
    QCOMPARE(outcome.items.first().pushVerdict,
             TimelineEvent::PushVerdict::Quiet);

    QJsonObject keyword = itemJson(QStringLiteral("b"), QStringLiteral("$b"),
                                   QStringLiteral("banana"));
    keyword.insert(QStringLiteral("push_notify"), true);
    keyword.insert(QStringLiteral("push_highlight"), true);
    QJsonObject append = diffJson(QStringLiteral("append"));
    append.insert(QStringLiteral("items"),
                  QJsonArray{keyword,
                             itemJson(QStringLiteral("c"), QStringLiteral("$c"),
                                      QStringLiteral("no verdict"))});
    outcome = applyTimelineDiff(mirror, append, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QCOMPARE(outcome.items.size(), 2);
    QCOMPARE(outcome.items.at(0).pushVerdict,
             TimelineEvent::PushVerdict::Notify);
    QVERIFY(outcome.items.at(0).pushHighlight);
    QCOMPARE(outcome.items.at(1).pushVerdict,
             TimelineEvent::PushVerdict::Unknown);

    QJsonObject legacy = itemJson(QStringLiteral("legacy"), QStringLiteral("$legacy"),
                                  QStringLiteral("@room update"));
    legacy.insert(QStringLiteral("mentions_room"), true);
    legacy.insert(QStringLiteral("room_mention_highlight"), true);
    legacy.insert(QStringLiteral("push_notify"), true);
    legacy.insert(QStringLiteral("push_highlight"), true);
    QJsonObject legacyPush = diffJson(QStringLiteral("push_back"));
    legacyPush.insert(QStringLiteral("item"), legacy);
    outcome = applyTimelineDiff(mirror, legacyPush, kRoom);
    QVERIFY(outcome.items.first().mentionsRoom);
    QVERIFY(outcome.items.first().roomMentionHighlighted);
}

void RustTimelineIngestTest::aResyncAppendCarriesTheBacklogMark()
{
    auto mirror = mirrorOf(1);
    QCOMPARE(applyTimelineDiff(mirror, diffJson(QStringLiteral("clear")), kRoom)
                 .kind,
             DiffOutcome::Cleared);
    QJsonObject history = itemJson(QStringLiteral("h"), QStringLiteral("$h"),
                                   QStringLiteral("old"));
    history.insert(QStringLiteral("push_notify"), true);
    history.insert(QStringLiteral("backlog"), true);
    QJsonObject append = diffJson(QStringLiteral("append"));
    append.insert(QStringLiteral("items"), QJsonArray{history});
    auto outcome = applyTimelineDiff(mirror, append, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QCOMPARE(outcome.items.size(), 1);
    QVERIFY(outcome.items.first().backlog);
    QCOMPARE(outcome.items.first().pushVerdict,
             TimelineEvent::PushVerdict::Notify);

    QJsonObject pushBack = diffJson(QStringLiteral("push_back"));
    pushBack.insert(QStringLiteral("item"),
                    itemJson(QStringLiteral("n"), QStringLiteral("$n"),
                             QStringLiteral("new")));
    outcome = applyTimelineDiff(mirror, pushBack, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Appended);
    QVERIFY(!outcome.items.first().backlog);
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
    QVERIFY(!e.body.isEmpty());
}

void RustTimelineIngestTest::parsesGalleryItemsBoundedAndClosed()
{
    QJsonObject row = itemJson(QStringLiteral("uidG"), QStringLiteral("$g"),
                               QString{});
    row.insert(QStringLiteral("msgtype"), QStringLiteral("image"));
    row.insert(QStringLiteral("media_filename"), QStringLiteral("before.png"));
    row.insert(QStringLiteral("media_key"), QStringLiteral("$g"));
    row.insert(QStringLiteral("media_source_available"), true);
    row.insert(QStringLiteral("gallery_items"), QJsonArray{
        QJsonObject{ { QStringLiteral("media_key"), QStringLiteral("$g") },
                     { QStringLiteral("kind"), QStringLiteral("image") },
                     { QStringLiteral("filename"), QStringLiteral("before.png") },
                     { QStringLiteral("mimetype"), QStringLiteral("image/png") },
                     { QStringLiteral("width"), 1280 },
                     { QStringLiteral("height"), 720 },
                     { QStringLiteral("size"), 482113 },
                     { QStringLiteral("thumb_available"), false } },
        QJsonObject{ { QStringLiteral("media_key"), QStringLiteral("$g#item1") },
                     { QStringLiteral("kind"), QStringLiteral("image") },
                     { QStringLiteral("filename"), QStringLiteral("after.png") },
                     { QStringLiteral("width"), 640 },
                     { QStringLiteral("height"), 480 },
                     { QStringLiteral("thumb_available"), true } },
        QJsonObject{ { QStringLiteral("kind"), QStringLiteral("image") } },
        QJsonObject{ { QStringLiteral("media_key"), QStringLiteral("$g#item3") },
                     { QStringLiteral("kind"), QStringLiteral("m.text") } },
        QStringLiteral("not an object"),
    });
    const TimelineEvent e = eventFromItemJson(row, kRoom);
    QCOMPARE(e.type, TimelineEvent::Image);
    QCOMPARE(e.body, QString{});
    QCOMPARE(e.galleryItems.size(), 2);
    QCOMPARE(e.galleryItems.at(0).mediaKey, QStringLiteral("$g"));
    QCOMPARE(e.galleryItems.at(0).mimetype, QStringLiteral("image/png"));
    QCOMPARE(e.galleryItems.at(0).size, qint64(482113));
    QCOMPARE(e.galleryItems.at(1).mediaKey, QStringLiteral("$g#item1"));
    QCOMPARE(e.galleryItems.at(1).filename, QStringLiteral("after.png"));
    QCOMPARE(e.galleryItems.at(1).width, 640);
    QVERIFY(e.galleryItems.at(1).thumbAvailable);

    QJsonObject lone = row;
    lone.insert(QStringLiteral("gallery_items"), QJsonArray{
        QJsonObject{ { QStringLiteral("media_key"), QStringLiteral("$g") },
                     { QStringLiteral("kind"), QStringLiteral("image") } } });
    QVERIFY(eventFromItemJson(lone, kRoom).galleryItems.isEmpty());

    QJsonArray many;
    for (int i = 0; i < 100; ++i)
        many.append(QJsonObject{
            { QStringLiteral("media_key"), QStringLiteral("$g#item%1").arg(i) },
            { QStringLiteral("kind"), QStringLiteral("file") } });
    QJsonObject flood = row;
    flood.insert(QStringLiteral("gallery_items"), many);
    QCOMPARE(eventFromItemJson(flood, kRoom).galleryItems.size(), 32);

    const QString longName(400, QLatin1Char('n'));
    const QString longMime(300, QLatin1Char('m'));
    QJsonObject named = row;
    named.insert(QStringLiteral("media_filename"), longName);
    named.insert(QStringLiteral("media_mimetype"), longMime);
    named.insert(QStringLiteral("gallery_items"), QJsonArray{
        QJsonObject{ { QStringLiteral("media_key"), QStringLiteral("$g") },
                     { QStringLiteral("kind"), QStringLiteral("image") },
                     { QStringLiteral("filename"), longName },
                     { QStringLiteral("mimetype"), longMime } },
        QJsonObject{ { QStringLiteral("media_key"), QStringLiteral("$g#item1") },
                     { QStringLiteral("kind"), QStringLiteral("image") } } });
    const TimelineEvent bounded = eventFromItemJson(named, kRoom);
    QCOMPARE(bounded.mediaFilename.size(), 255);
    QCOMPARE(bounded.mediaMimetype.size(), 127);
    QCOMPARE(bounded.galleryItems.at(0).filename.size(), 255);
    QCOMPARE(bounded.galleryItems.at(0).mimetype.size(), 127);
    named.insert(QStringLiteral("body"), longName);
    const TimelineEvent echoed = eventFromItemJson(named, kRoom);
    QCOMPARE(echoed.body, echoed.mediaFilename);
    named.insert(QStringLiteral("body"), QStringLiteral("look at this"));
    QCOMPARE(eventFromItemJson(named, kRoom).body, QStringLiteral("look at this"));
}

void RustTimelineIngestTest::mediaFilenameLosesBidiControls()
{
    QJsonObject row = itemJson(QStringLiteral("uidB"), QStringLiteral("$b"),
                               QStringLiteral("hello"));
    row.insert(QStringLiteral("media_mxc"), QStringLiteral("mxc://example.org/a"));
    row.insert(QStringLiteral("media_filename"),
               QStringLiteral("r\u202Etxt.exe\u202C.txt"));
    QCOMPARE(eventFromItemJson(row, kRoom).mediaFilename,
             QStringLiteral("rtxt.exe.txt"));
    row.insert(QStringLiteral("body"), QStringLiteral("a\u202Eb"));
    QCOMPARE(eventFromItemJson(row, kRoom).body, QStringLiteral("a\u202Eb"));
}

void RustTimelineIngestTest::parsesReplyTargetKindAndCount()
{
    QJsonObject reply = itemJson(QStringLiteral("uidR"), QStringLiteral("$r"),
                                 QStringLiteral("nice"));
    reply.insert(QStringLiteral("reply_to_event_id"), QStringLiteral("$g"));
    reply.insert(QStringLiteral("reply_to_preview"), QString{});
    reply.insert(QStringLiteral("reply_to_kind"), QStringLiteral("image"));
    reply.insert(QStringLiteral("reply_to_count"), 2);
    const TimelineEvent e = eventFromItemJson(reply, kRoom);
    QCOMPARE(e.replyToKind, QStringLiteral("image"));
    QCOMPARE(e.replyToCount, 2);

    reply.insert(QStringLiteral("reply_to_kind"), QStringLiteral("<b>x</b>"));
    reply.insert(QStringLiteral("reply_to_count"), 100000);
    const TimelineEvent bad = eventFromItemJson(reply, kRoom);
    QCOMPARE(bad.replyToKind, QString{});
    QCOMPARE(bad.replyToCount, 32);
    reply.insert(QStringLiteral("reply_to_count"), -4);
    QCOMPARE(eventFromItemJson(reply, kRoom).replyToCount, 0);
}

void RustTimelineIngestTest::parsesTypedMediaItems()
{
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

    QJsonObject audio = itemJson(QStringLiteral("uidA"), QStringLiteral("$a"),
                                 QStringLiteral("song.ogg"));
    audio.insert(QStringLiteral("msgtype"), QStringLiteral("audio"));
    audio.insert(QStringLiteral("media_duration_ms"), 4000);
    const TimelineEvent a = eventFromItemJson(audio, kRoom);
    QCOMPARE(a.type, TimelineEvent::Audio);
    QCOMPARE(a.mediaDurationMs, qint64(4000));
    QVERIFY(!a.mediaIsVoice);

    QJsonObject voice = audio;
    voice.insert(QStringLiteral("media_voice"), true);
    voice.insert(QStringLiteral("media_waveform"),
                 QJsonArray{ 0, 50, 100, 101, -3, QStringLiteral("x") });
    const TimelineEvent vo = eventFromItemJson(voice, kRoom);
    QCOMPARE(vo.type, TimelineEvent::Audio);
    QVERIFY(vo.mediaIsVoice);
    QCOMPARE(vo.mediaWaveform, (QList<int>{ 0, 50, 100 }));

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

}

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
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().itemId, QStringLiteral("uidP"));
    QCOMPARE(mirror.first().pollAnswers.at(1).count, 2);
    QCOMPARE(mirror.first().pollTotalVoters, 4);

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

void RustTimelineIngestTest::parsesMediaUploadProgress()
{
    QJsonObject item = itemJson(QStringLiteral("uid-upload"), QString(),
                                QStringLiteral("outgoing"));
    item.insert(QStringLiteral("transaction_id"), QStringLiteral("txn-up"));
    item.insert(QStringLiteral("is_local_echo"), true);
    item.insert(QStringLiteral("send_state"), QStringLiteral("sending"));
    TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.uploadedBytes, 0);
    QCOMPARE(e.uploadTotalBytes, 0);

    item.insert(QStringLiteral("send_upload_current"), 512);
    item.insert(QStringLiteral("send_upload_total"), 2048);
    e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.uploadedBytes, 512);
    QCOMPARE(e.uploadTotalBytes, 2048);

    item.insert(QStringLiteral("send_upload_current"), 2048);
    e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.uploadedBytes, 2048);
    QCOMPARE(e.status, TimelineEvent::Sending);
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

    item.insert(QStringLiteral("reply_to_preview"),
                QStringLiteral("[Grok AI](https://matrix.to/#/"
                               "@brotato:example.org) tai jo"));
    const TimelineEvent m = eventFromItemJson(item, kRoom);
    QCOMPARE(m.replyToPreview, QStringLiteral("Grok AI tai jo"));
}

void RustTimelineIngestTest::parsesReactionSenders()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QStringLiteral("hello"));
    QJsonArray senders;
    senders.append(QStringLiteral("@me:example.org"));
    senders.append(QStringLiteral("@bob:example.org"));
    senders.append(QString());
    senders.append(QJsonValue(7));
    senders.append(QStringLiteral("@carol:example.org"));

    QJsonObject up;
    up.insert(QStringLiteral("key"), QStringLiteral("👍"));
    up.insert(QStringLiteral("count"), 250);
    up.insert(QStringLiteral("by_me"), true);
    up.insert(QStringLiteral("senders"), senders);
    QJsonArray reactions;
    reactions.append(up);
    item.insert(QStringLiteral("reactions"), reactions);

    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.reactions.size(), 1);
    const Reaction &r = e.reactions.first();
    QCOMPARE(r.count, 250);
    QCOMPARE(r.senders.size(), 3);
    QCOMPARE(r.senders.at(0), QStringLiteral("@me:example.org"));
    QCOMPARE(r.senders.at(1), QStringLiteral("@bob:example.org"));
    QCOMPARE(r.senders.at(2), QStringLiteral("@carol:example.org"));
}

// Backends without reactor identities (mock, HTTP, older Rust) give an empty
// list beside a real count, meaning "unknown", never "nobody".
void RustTimelineIngestTest::reactionSendersAbsentLeavesEmptyList()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QStringLiteral("hello"));
    QJsonObject up;
    up.insert(QStringLiteral("key"), QStringLiteral("👍"));
    up.insert(QStringLiteral("count"), 2);
    up.insert(QStringLiteral("by_me"), false);
    QJsonArray reactions;
    reactions.append(up);
    item.insert(QStringLiteral("reactions"), reactions);

    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.reactions.size(), 1);
    QCOMPARE(e.reactions.first().count, 2);
    QVERIFY(e.reactions.first().senders.isEmpty());
}

void RustTimelineIngestTest::parsesReadReceipts()
{
    QJsonObject item = itemJson(QStringLiteral("uid1"), QStringLiteral("$ev1"),
                                QStringLiteral("hello"));
    QJsonArray readBy;
    QJsonObject bob;
    bob.insert(QStringLiteral("user_id"), QStringLiteral("@bob:example.org"));
    bob.insert(QStringLiteral("ts"), 1700000123000.0);
    readBy.append(bob);
    QJsonObject carol;
    carol.insert(QStringLiteral("user_id"),
                 QStringLiteral("@carol:example.org"));
    carol.insert(QStringLiteral("ts"), QJsonValue::Null);
    readBy.append(carol);
    QJsonObject noUser;
    noUser.insert(QStringLiteral("ts"), 1700000000000.0);
    readBy.append(noUser);
    readBy.append(QStringLiteral("not an object"));
    item.insert(QStringLiteral("read_by"), readBy);

    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.readBy.size(), 2);
    QCOMPARE(e.readBy.at(0).userId, QStringLiteral("@bob:example.org"));
    QCOMPARE(e.readBy.at(0).tsMs, Q_INT64_C(1700000123000));
    QCOMPARE(e.readBy.at(1).userId, QStringLiteral("@carol:example.org"));
    QCOMPARE(e.readBy.at(1).tsMs, Q_INT64_C(0));
    QCOMPARE(e.readByTotal, 2);

    item.insert(QStringLiteral("read_by_total"), 40);
    QCOMPARE(eventFromItemJson(item, kRoom).readByTotal, 40);
    item.insert(QStringLiteral("read_by_total"), 1);
    QCOMPARE(eventFromItemJson(item, kRoom).readByTotal, 2);

    const TimelineEvent plain = eventFromItemJson(
        itemJson(QStringLiteral("uid2"), QStringLiteral("$ev2"),
                 QStringLiteral("plain")), kRoom);
    QVERIFY(plain.readBy.isEmpty());
    QCOMPARE(plain.readByTotal, 0);
}

void RustTimelineIngestTest::readReceiptsMoveBetweenRowsViaSet()
{
    QJsonObject older = itemJson(QStringLiteral("uidA"), QStringLiteral("$a"),
                                 QStringLiteral("first"));
    QJsonObject bobReceipt;
    bobReceipt.insert(QStringLiteral("user_id"),
                      QStringLiteral("@bob:example.org"));
    bobReceipt.insert(QStringLiteral("ts"), 1700000001000.0);
    older.insert(QStringLiteral("read_by"), QJsonArray{ bobReceipt });
    QJsonArray items;
    items.append(older);
    items.append(itemJson(QStringLiteral("uidB"), QStringLiteral("$b"),
                          QStringLiteral("second")));
    auto mirror = eventsFromItemArray(items, kRoom);
    QCOMPARE(mirror.at(0).readBy.size(), 1);
    QVERIFY(mirror.at(1).readBy.isEmpty());

    QJsonObject clearOld = diffJson(QStringLiteral("set"));
    clearOld.insert(QStringLiteral("index"), 0);
    clearOld.insert(QStringLiteral("item"),
                    itemJson(QStringLiteral("uidA"), QStringLiteral("$a"),
                             QStringLiteral("first")));
    QCOMPARE(applyTimelineDiff(mirror, clearOld, kRoom).kind,
             DiffOutcome::Changed);

    QJsonObject newer = itemJson(QStringLiteral("uidB"), QStringLiteral("$b"),
                                 QStringLiteral("second"));
    bobReceipt.insert(QStringLiteral("ts"), 1700000002000.0);
    newer.insert(QStringLiteral("read_by"), QJsonArray{ bobReceipt });
    QJsonObject setNew = diffJson(QStringLiteral("set"));
    setNew.insert(QStringLiteral("index"), 1);
    setNew.insert(QStringLiteral("item"), newer);
    QCOMPARE(applyTimelineDiff(mirror, setNew, kRoom).kind,
             DiffOutcome::Changed);

    QCOMPARE(mirror.size(), 2);
    QVERIFY(mirror.at(0).readBy.isEmpty());
    QCOMPARE(mirror.at(1).readBy.size(), 1);
    QCOMPARE(mirror.at(1).readBy.first().userId,
             QStringLiteral("@bob:example.org"));
    QCOMPARE(mirror.at(1).readBy.first().tsMs, Q_INT64_C(1700000002000));
}

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

    item.insert(QStringLiteral("thread_latest_preview"),
                QStringLiteral("[Grok AI](https://matrix.to/#/"
                               "@brotato:example.org) ok"));
    const TimelineEvent m = eventFromItemJson(item, kRoom);
    QCOMPARE(m.threadLatestPreview, QStringLiteral("Grok AI ok"));
}

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

void RustTimelineIngestTest::threadPanelIngestPreservesReplies()
{
    const QString root = QStringLiteral("$root");
    QJsonArray items;
    QJsonObject rootItem = itemJson(QStringLiteral("uid-root"), root,
                                    QStringLiteral("root message"));
    rootItem.insert(QStringLiteral("is_thread_root"), true);
    rootItem.insert(QStringLiteral("thread_reply_count"), 3);
    items.append(rootItem);
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
    QVERIFY(mirror.first().threadRootId.isEmpty());
    int replies = 0;
    for (const auto &e : mirror) {
        if (e.threadRootId == root) {
            ++replies;
            QVERIFY(!e.isThreadRoot);
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

void RustTimelineIngestTest::parsesTypedProfileChange()
{
    QJsonObject item = itemJson(QStringLiteral("profile-1"),
                                QStringLiteral("$profile"), QString());
    item.insert(QStringLiteral("msgtype"), QStringLiteral("state"));
    item.insert(QStringLiteral("state_kind"), QStringLiteral("member_profile"));
    item.insert(QStringLiteral("state_target"),
                QStringLiteral("@alice:example.org"));
    item.insert(QStringLiteral("profile_name_change"),
                QStringLiteral("changed"));
    item.insert(QStringLiteral("profile_name_old"), QStringLiteral("Alice"));
    item.insert(QStringLiteral("profile_name_new"), QStringLiteral("Alice A."));
    item.insert(QStringLiteral("profile_avatar_changed"), true);

    const TimelineEvent changed = eventFromItemJson(item, kRoom);
    QCOMPARE(changed.type, TimelineEvent::StateChange);
    QCOMPARE(changed.stateKind, QStringLiteral("member_profile"));
    QCOMPARE(changed.stateTarget, QStringLiteral("@alice:example.org"));
    QCOMPARE(changed.profileNameChange, QStringLiteral("changed"));
    QCOMPARE(changed.profileNameOld, QStringLiteral("Alice"));
    QCOMPARE(changed.profileNameNew, QStringLiteral("Alice A."));
    QVERIFY(changed.profileAvatarChanged);
    QVERIFY(changed.body.isEmpty());

    item.remove(QStringLiteral("profile_name_old"));
    item.insert(QStringLiteral("profile_name_change"), QStringLiteral("set"));
    item.insert(QStringLiteral("profile_avatar_changed"), false);
    const TimelineEvent set = eventFromItemJson(item, kRoom);
    QCOMPARE(set.profileNameChange, QStringLiteral("set"));
    QVERIFY(set.profileNameOld.isEmpty());
    QCOMPARE(set.profileNameNew, QStringLiteral("Alice A."));
    QVERIFY(!set.profileAvatarChanged);

    item.remove(QStringLiteral("profile_name_new"));
    item.insert(QStringLiteral("profile_name_old"), QStringLiteral("Alice"));
    item.insert(QStringLiteral("profile_name_change"),
                QStringLiteral("cleared"));
    const TimelineEvent cleared = eventFromItemJson(item, kRoom);
    QCOMPARE(cleared.profileNameChange, QStringLiteral("cleared"));
    QCOMPARE(cleared.profileNameOld, QStringLiteral("Alice"));
    QVERIFY(cleared.profileNameNew.isEmpty());

    item.remove(QStringLiteral("profile_name_old"));
    item.insert(QStringLiteral("profile_name_change"), QJsonValue::Null);
    item.insert(QStringLiteral("profile_avatar_changed"), true);
    const TimelineEvent avatarOnly = eventFromItemJson(item, kRoom);
    QVERIFY(avatarOnly.profileNameChange.isEmpty());
    QVERIFY(avatarOnly.profileNameOld.isEmpty());
    QVERIFY(avatarOnly.profileNameNew.isEmpty());
    QVERIFY(avatarOnly.profileAvatarChanged);
}

// Other row kinds and the mock/HTTP backends send none of these fields; the
// struct defaults are kept.
void RustTimelineIngestTest::profileChangeAbsentFieldsKeepDefaults()
{
    const QJsonObject item = itemJson(QStringLiteral("uid1"),
                                      QStringLiteral("$ev1"),
                                      QStringLiteral("hello"));
    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QVERIFY(e.profileNameChange.isEmpty());
    QVERIFY(e.profileNameOld.isEmpty());
    QVERIFY(e.profileNameNew.isEmpty());
    QVERIFY(!e.profileAvatarChanged);
}

void RustTimelineIngestTest::profileNameIsBoundedWithoutSplittingSurrogatePairs()
{
    QString cloud;
    cloud.append(QChar(QChar::highSurrogate(0x1F329)));
    cloud.append(QChar(QChar::lowSurrogate(0x1F329)));
    QCOMPARE(cloud.size(), 2);
    const QString overlong = cloud.repeated(300);

    QJsonObject item = itemJson(QStringLiteral("profile-1"),
                                QStringLiteral("$profile"), QString());
    item.insert(QStringLiteral("msgtype"), QStringLiteral("state"));
    item.insert(QStringLiteral("state_kind"), QStringLiteral("member_profile"));
    item.insert(QStringLiteral("profile_name_change"),
                QStringLiteral("changed"));
    item.insert(QStringLiteral("profile_name_old"), overlong);
    item.insert(QStringLiteral("profile_name_new"), overlong);

    const TimelineEvent e = eventFromItemJson(item, kRoom);
    QCOMPARE(e.profileNameNew.toUcs4().size(), 255);
    QCOMPARE(e.profileNameNew.size(), 510);
    QCOMPARE(e.profileNameOld.toUcs4().size(), 255);
    QVERIFY(e.profileNameNew.at(e.profileNameNew.size() - 1).isLowSurrogate());
    const auto points = e.profileNameNew.toUcs4();
    QCOMPARE(points.first(), 0x1F329u);
    QCOMPARE(points.last(), 0x1F329u);

    const QString exact = cloud.repeated(255);
    item.insert(QStringLiteral("profile_name_new"), exact);
    const TimelineEvent atBound = eventFromItemJson(item, kRoom);
    QCOMPARE(atBound.profileNameNew, exact);

    item.insert(QStringLiteral("profile_name_new"),
                QString(300, QLatin1Char('a')));
    const TimelineEvent ascii = eventFromItemJson(item, kRoom);
    QCOMPARE(ascii.profileNameNew.size(), 255);
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
    auto item = diff.value(QStringLiteral("item")).toObject();
    item.insert(QStringLiteral("edited"), true);
    item.insert(QStringLiteral("formatted_body"), QStringLiteral("<b>edited body</b>"));
    diff.insert(QStringLiteral("item"), item);
    const auto outcome = applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(outcome.index, 2);
    QCOMPARE(mirror.size(), 3);
    QCOMPARE(mirror.at(2).eventId, QStringLiteral("$ev2"));
    QCOMPARE(mirror.at(2).itemId, QStringLiteral("item2"));
    QCOMPARE(mirror.at(2).body, QStringLiteral("edited body"));
    QCOMPARE(mirror.at(2).formattedBody, QStringLiteral("<b>edited body</b>"));
    QVERIFY(mirror.at(2).edited);
    item.insert(QStringLiteral("body"), QStringLiteral("second edit"));
    item.remove(QStringLiteral("formatted_body"));
    diff.insert(QStringLiteral("item"), item);
    applyTimelineDiff(mirror, diff, kRoom);
    QCOMPARE(mirror.at(2).body, QStringLiteral("second edit"));
    QVERIFY(mirror.at(2).formattedBody.isEmpty());
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
    QCOMPARE(mirror.size(), 2);
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
    QCOMPARE(mirror.size(), 1);
    QVERIFY(!mirror.first().undecryptable);
    QVERIFY(mirror.first().isDecrypted);
    QCOMPARE(mirror.first().body, QStringLiteral("now readable"));
    QCOMPARE(mirror.first().eventId, QStringLiteral("$enc"));
    QCOMPARE(mirror.first().itemId, QStringLiteral("uid1"));
}

void RustTimelineIngestTest::localEchoReconciledViaSet()
{
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

    QJsonObject remote = itemJson(QStringLiteral("uid-local"),
                                  QStringLiteral("$server"),
                                  QStringLiteral("outgoing"));
    remote.insert(QStringLiteral("send_state"), QStringLiteral("sent"));
    QJsonObject set = diffJson(QStringLiteral("set"));
    set.insert(QStringLiteral("index"), 0);
    set.insert(QStringLiteral("item"), remote);
    QCOMPARE(applyTimelineDiff(mirror, set, kRoom).kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().eventId, QStringLiteral("$server"));
    QCOMPARE(mirror.first().status, TimelineEvent::Sent);
}

void RustTimelineIngestTest::trackerAdoptsRequestedRoomOnly()
{
    TimelineGenerationTracker tracker;
    tracker.request(kRoom);
    QVERIFY(!tracker.readyForPagination(kRoom));
    QVERIFY(!tracker.adoptReset(QStringLiteral("!other:example.org"), 4));
    QVERIFY(!tracker.hasActiveTimeline());
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
    QVERIFY(!tracker.accepts(kRoom, 6));
    QVERIFY(!tracker.accepts(QStringLiteral("!other:example.org"), 7));
    QVERIFY(!tracker.adoptReset(kRoom, 0));
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
    tracker.reset();
    QVERIFY(!tracker.hasActiveTimeline());
    QVERIFY(!tracker.accepts(kRoom, 2));
    QVERIFY(!tracker.adoptReset(kRoom, 3));
}

void RustTimelineIngestTest::emptyThreadResetThenAppendPopulates()
{
    const QString threadId = QStringLiteral("!room:example.org|$root");
    TimelineGenerationTracker tracker;
    tracker.request(threadId);
    QVERIFY(tracker.adoptReset(threadId, 3));
    QList<TimelineEvent> mirror;
    QVERIFY(mirror.isEmpty());

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

    QVERIFY(!tracker.accepts(threadId, 2));
}

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

    QJsonObject set = diffJson(QStringLiteral("set"));
    set.insert(QStringLiteral("index"), 0);
    set.insert(QStringLiteral("item"),
               threadRootItem(root, 2, QStringLiteral("image"),
                              QStringLiteral("photo.png"),
                              QStringLiteral("@carol:example.org")));
    auto outcome = applyTimelineDiff(mirror, set, kRoom);
    QCOMPARE(outcome.kind, DiffOutcome::Changed);
    QCOMPARE(mirror.size(), 1);
    QVERIFY(mirror.first().isThreadRoot);
    QCOMPARE(mirror.first().threadReplyCount, 2);
    QCOMPARE(mirror.first().threadLatestKind, QStringLiteral("image"));
    QCOMPARE(mirror.first().threadLatestSender,
             QStringLiteral("@carol:example.org"));

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
    QCOMPARE(mirror.size(), 1);
    QCOMPARE(mirror.first().threadLatestKind, QStringLiteral("text"));
    QCOMPARE(mirror.first().threadLatestPreview, QStringLiteral("now readable"));

    TimelineGenerationTracker tracker;
    tracker.request(kRoom);
    QVERIFY(tracker.adoptReset(kRoom, 5));
    QVERIFY(!tracker.accepts(kRoom, 4));
    QVERIFY(tracker.accepts(kRoom, 5));
}

void RustTimelineIngestTest::membersFromPayloadBuildsCacheEntries()
{
    QJsonArray rows;
    QJsonObject maya;
    maya.insert(QStringLiteral("user_id"), QStringLiteral("@maya:example.org"));
    maya.insert(QStringLiteral("display_name"), QStringLiteral("Maya Chen"));
    maya.insert(QStringLiteral("avatar_url"),
                QStringLiteral("mxc://example.org/maya"));
    rows.append(maya);
    QJsonObject unnamed;
    unnamed.insert(QStringLiteral("user_id"),
                   QStringLiteral("@ghost:example.org"));
    rows.append(unnamed);
    QJsonObject invalid;
    invalid.insert(QStringLiteral("display_name"), QStringLiteral("Nobody"));
    rows.append(invalid);

    const auto members = matrix::rust_timeline::membersFromPayload(rows);
    QCOMPARE(members.size(), 2);
    QCOMPARE(members.value(QStringLiteral("@maya:example.org")).displayName,
             QStringLiteral("Maya Chen"));
    QCOMPARE(members.value(QStringLiteral("@maya:example.org")).avatarMxcUrl,
             QStringLiteral("mxc://example.org/maya"));
    QVERIFY(members.contains(QStringLiteral("@ghost:example.org")));
    QVERIFY(members.value(QStringLiteral("@ghost:example.org"))
                .displayName.isEmpty());
}

void RustTimelineIngestTest::aSnapshotDropsALocalEchoTheRemoteEventAlreadyCovers()
{
    const QString room = QStringLiteral("!r:example.org");
    auto item = [](const QString &eventId, const QString &txn, bool echo,
                   const QString &body) {
        QJsonObject o{
            { QStringLiteral("item_id"), QStringLiteral("i_") + eventId + txn },
            { QStringLiteral("kind"), QStringLiteral("event") },
            { QStringLiteral("event_id"), eventId },
            { QStringLiteral("transaction_id"), txn },
            { QStringLiteral("sender"), QStringLiteral("@me:example.org") },
            { QStringLiteral("timestamp_ms"), 1700000000000LL },
            { QStringLiteral("is_own"), true },
            { QStringLiteral("is_local_echo"), echo },
            { QStringLiteral("msgtype"), QStringLiteral("m.text") },
            { QStringLiteral("body"), body },
        };
        return o;
    };

    QJsonArray items;
    items.append(item(QStringLiteral("$old"), QString(), false,
                      QStringLiteral("earlier")));
    items.append(item(QStringLiteral("$dup"), QString(), false,
                      QStringLiteral("hello")));
    items.append(item(QStringLiteral("$dup"), QStringLiteral("txn1"), true,
                      QStringLiteral("hello")));

    const QList<TimelineEvent> out =
        matrix::rust_timeline::eventsFromItemArray(items, room);

    QCOMPARE(out.size(), 2);
    QCOMPARE(out.at(1).eventId, QStringLiteral("$dup"));
    QVERIFY2(!out.at(1).isLocalEcho,
             "the echo was kept and the remote row dropped; the remote row "
             "carries the server timestamp and the authoritative state");
    QCOMPARE(out.at(0).eventId, QStringLiteral("$old"));

    QJsonArray pending;
    pending.append(item(QStringLiteral("$old"), QString(), false,
                        QStringLiteral("earlier")));
    pending.append(item(QString(), QStringLiteral("txn2"), true,
                        QStringLiteral("still sending")));
    const QList<TimelineEvent> kept =
        matrix::rust_timeline::eventsFromItemArray(pending, room);
    QCOMPARE(kept.size(), 2);
    QVERIFY(kept.at(1).isLocalEcho);

    QJsonArray lone;
    lone.append(item(QStringLiteral("$other"), QString(), false,
                     QStringLiteral("earlier")));
    lone.append(item(QStringLiteral("$mine"), QStringLiteral("txn3"), true,
                     QStringLiteral("sent, not yet synced")));
    QCOMPARE(matrix::rust_timeline::eventsFromItemArray(lone, room).size(), 2);
}

namespace {

QJsonObject syncPayload(const QString &eventId, const QString &body)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("event_id"), eventId);
    obj.insert(QStringLiteral("sender"), QStringLiteral("@bob:example.org"));
    obj.insert(QStringLiteral("body"), body);
    obj.insert(QStringLiteral("msgtype"), QStringLiteral("text"));
    obj.insert(QStringLiteral("timestamp_ms"), 1700000000000.0);
    obj.insert(QStringLiteral("is_encrypted"), false);
    obj.insert(QStringLiteral("is_decrypted"), false);
    obj.insert(QStringLiteral("undecryptable"), false);
    obj.insert(QStringLiteral("mentions_me"), false);
    obj.insert(QStringLiteral("mentions_room"), false);
    obj.insert(QStringLiteral("thread_root_id"), QString());
    return obj;
}

QJsonObject echoItemJson(const QString &itemId, const QString &body)
{
    QJsonObject item;
    item.insert(QStringLiteral("item_id"), itemId);
    item.insert(QStringLiteral("kind"), QStringLiteral("event"));
    item.insert(QStringLiteral("transaction_id"), itemId);
    item.insert(QStringLiteral("sender"), QStringLiteral("@me:example.org"));
    item.insert(QStringLiteral("msgtype"), QStringLiteral("text"));
    item.insert(QStringLiteral("body"), body);
    item.insert(QStringLiteral("is_local_echo"), true);
    item.insert(QStringLiteral("send_state"), QStringLiteral("failed"));
    item.insert(QStringLiteral("timestamp_ms"), 1700000000500.0);
    return item;
}

QJsonObject insertDiff(int index, const QJsonObject &item)
{
    QJsonObject diff = diffJson(QStringLiteral("insert"));
    diff.insert(QStringLiteral("index"), index);
    diff.insert(QStringLiteral("item"), item);
    return diff;
}

}

void RustTimelineIngestTest::readsThePushSoundTweakOnlyWithAVerdict()
{
    TimelineEvent loud;
    readPushVerdict(QJsonObject{ { QStringLiteral("push_notify"), true },
                                 { QStringLiteral("push_highlight"), false },
                                 { QStringLiteral("push_sound"), true } },
                    loud);
    QVERIFY2(loud.pushSound, "a Loud verdict lost its sound tweak");

    TimelineEvent notifyOnly;
    readPushVerdict(QJsonObject{ { QStringLiteral("push_notify"), true } },
                    notifyOnly);
    QVERIFY(!notifyOnly.pushSound);

    TimelineEvent none;
    readPushVerdict(QJsonObject{ { QStringLiteral("push_sound"), true } }, none);
    QCOMPARE(none.pushVerdict, TimelineEvent::PushVerdict::Unknown);
    QVERIFY(!none.pushSound);
}

void RustTimelineIngestTest::aSyncPayloadCarriesBacklogReplyAndEditFacts()
{
    QJsonObject obj = syncPayload(QStringLiteral("$edit"), QStringLiteral("* fixed"));
    obj.insert(QStringLiteral("backlog"), true);
    obj.insert(QStringLiteral("in_reply_to"), QStringLiteral("$parent"));
    obj.insert(QStringLiteral("replaces_event_id"), QStringLiteral("$orig"));
    obj.insert(QStringLiteral("new_body"), QStringLiteral("fixed"));
    obj.insert(QStringLiteral("push_notify"), true);
    obj.insert(QStringLiteral("push_highlight"), true);
    obj.insert(QStringLiteral("push_sound"), true);
    obj.insert(QStringLiteral("room_mention_highlight"), true);

    const TimelineEvent e = matrix::rust_timeline::syncEventFromPayload(obj, kRoom);
    QCOMPARE(e.eventId, QStringLiteral("$edit"));
    QCOMPARE(e.roomId, kRoom);
    QVERIFY(e.backlog);
    QCOMPARE(e.replyToEventId, QStringLiteral("$parent"));
    QVERIFY(e.isEdit);
    QVERIFY(e.roomMentionHighlighted);
    QCOMPARE(e.replacesEventId, QStringLiteral("$orig"));
    QCOMPARE(e.editNewBody, QStringLiteral("fixed"));
    QVERIFY(e.editNewBodyAvailable);
    auto emptyEdit = obj;
    emptyEdit.insert(QStringLiteral("new_body"), QStringLiteral(""));
    const auto empty = matrix::rust_timeline::syncEventFromPayload(emptyEdit, kRoom);
    QVERIFY(empty.editNewBodyAvailable);
    QVERIFY(empty.editNewBody.isEmpty());
    emptyEdit.remove(QStringLiteral("new_body"));
    QVERIFY(!matrix::rust_timeline::syncEventFromPayload(emptyEdit, kRoom).editNewBodyAvailable);
    QCOMPARE(e.body, QStringLiteral("* fixed"));
    QCOMPARE(e.pushVerdict, TimelineEvent::PushVerdict::Notify);
    QVERIFY(e.pushHighlight);
    QVERIFY(e.pushSound);

    const TimelineEvent plain = matrix::rust_timeline::syncEventFromPayload(
        syncPayload(QStringLiteral("$plain"), QStringLiteral("hi")), kRoom);
    QVERIFY(!plain.backlog);
    QVERIFY(!plain.isEdit);
    QVERIFY(plain.replacesEventId.isEmpty());
    QVERIFY(plain.editNewBody.isEmpty());
    QVERIFY(plain.replyToEventId.isEmpty());
    QCOMPARE(plain.type, TimelineEvent::TextMessage);
    QJsonObject custom = syncPayload(QStringLiteral("$c"), QStringLiteral("x"));
    custom.insert(QStringLiteral("msgtype"), QStringLiteral("org.example.custom"));
    QCOMPARE(matrix::rust_timeline::syncEventFromPayload(custom, kRoom).type,
             TimelineEvent::TextMessage);
}

void RustTimelineIngestTest::anUndecryptableSyncPayloadKeepsItsThreadRoot()
{
    QJsonObject obj = syncPayload(QStringLiteral("$utd"), QString());
    obj.insert(QStringLiteral("undecryptable"), true);
    obj.insert(QStringLiteral("is_encrypted"), true);
    obj.insert(QStringLiteral("msgtype"), QStringLiteral("encrypted"));
    obj.insert(QStringLiteral("thread_root_id"), QStringLiteral("$root"));
    const TimelineEvent e = matrix::rust_timeline::syncEventFromPayload(obj, kRoom);
    QVERIFY(e.undecryptable);
    QVERIFY(e.isEncrypted);
    QCOMPARE(e.threadRootId, QStringLiteral("$root"));
    QCOMPARE(matrix::rust_timeline::syncRouteFor(false, e),
             matrix::rust_timeline::SyncRoute::AppendUnmirrored);
}

void RustTimelineIngestTest::aSyncPollStartIsAPollRow()
{
    QJsonObject obj = syncPayload(QStringLiteral("$poll"),
                                  QStringLiteral("Lunch?\n1. Pizza\n2. Soup"));
    obj.insert(QStringLiteral("msgtype"), QStringLiteral("poll"));
    obj.insert(QStringLiteral("poll_question"), QStringLiteral("Lunch?"));
    const TimelineEvent e = matrix::rust_timeline::syncEventFromPayload(obj, kRoom);
    QCOMPARE(e.type, TimelineEvent::Poll);
    QCOMPARE(e.pollQuestion, QStringLiteral("Lunch?"));
    QCOMPARE(matrix::rust_timeline::syncRouteFor(false, e),
             matrix::rust_timeline::SyncRoute::AppendUnmirrored);
}

void RustTimelineIngestTest::theOpenRoomsThreadReplyIsObservedNeverAppended()
{
    using matrix::rust_timeline::SyncRoute;
    using matrix::rust_timeline::syncRouteFor;
    QJsonObject reply = syncPayload(QStringLiteral("$reply"), QStringLiteral("hi"));
    reply.insert(QStringLiteral("thread_root_id"), QStringLiteral("$root"));
    const TimelineEvent threaded =
        matrix::rust_timeline::syncEventFromPayload(reply, kRoom);
    QCOMPARE(syncRouteFor(true, threaded), SyncRoute::ObserveOnly);
    QCOMPARE(syncRouteFor(false, threaded), SyncRoute::AppendUnmirrored);

    QJsonObject root = syncPayload(QStringLiteral("$root"), QStringLiteral("root"));
    root.insert(QStringLiteral("thread_root_id"), QStringLiteral("$root"));
    const TimelineEvent rootEvent =
        matrix::rust_timeline::syncEventFromPayload(root, kRoom);
    QCOMPARE(syncRouteFor(true, rootEvent), SyncRoute::PreviewOnly);
    QCOMPARE(syncRouteFor(false, rootEvent), SyncRoute::MirrorAndAppend);

    reply.insert(QStringLiteral("replaces_event_id"), QStringLiteral("$older"));
    const TimelineEvent threadedEdit =
        matrix::rust_timeline::syncEventFromPayload(reply, kRoom);
    QCOMPARE(syncRouteFor(true, threadedEdit), SyncRoute::PreviewOnly);

    const TimelineEvent plain = matrix::rust_timeline::syncEventFromPayload(
        syncPayload(QStringLiteral("$p"), QStringLiteral("p")), kRoom);
    QCOMPARE(syncRouteFor(true, plain), SyncRoute::PreviewOnly);
}

void RustTimelineIngestTest::aRemoteInsertAboveAPendingSendIsALiveArrival()
{
    QJsonArray items;
    items.append(itemJson(QStringLiteral("i0"), QStringLiteral("$a"), QStringLiteral("a")));
    items.append(itemJson(QStringLiteral("i1"), QStringLiteral("$b"), QStringLiteral("b")));
    items.append(echoItemJson(QStringLiteral("txn1"), QStringLiteral("mine")));
    QList<TimelineEvent> mirror = eventsFromItemArray(items, kRoom);
    QCOMPARE(mirror.size(), 3);

    const DiffOutcome out = applyTimelineDiff(
        mirror,
        insertDiff(2, itemJson(QStringLiteral("i2"), QStringLiteral("$c"),
                               QStringLiteral("new"))),
        kRoom);
    QCOMPARE(out.kind, DiffOutcome::Inserted);
    QCOMPARE(out.index, 2);
    QVERIFY2(out.liveRemoteInsert,
             "a message landing above a failed send was not a live arrival");
    QCOMPARE(mirror.at(2).eventId, QStringLiteral("$c"));
    QVERIFY(mirror.at(3).isLocalEcho);

    const DiffOutcome echo = applyTimelineDiff(
        mirror, insertDiff(3, echoItemJson(QStringLiteral("txn2"), QStringLiteral("x"))),
        kRoom);
    QCOMPARE(echo.kind, DiffOutcome::Inserted);
    QVERIFY(!echo.liveRemoteInsert);
}

void RustTimelineIngestTest::aPaginationInsertIsNeverALiveArrival()
{
    QJsonArray items;
    QJsonObject start;
    start.insert(QStringLiteral("item_id"), QStringLiteral("start"));
    start.insert(QStringLiteral("kind"), QStringLiteral("timeline_start"));
    items.append(start);
    items.append(itemJson(QStringLiteral("i0"), QStringLiteral("$a"), QStringLiteral("a")));
    items.append(echoItemJson(QStringLiteral("txn1"), QStringLiteral("mine")));
    QList<TimelineEvent> mirror = eventsFromItemArray(items, kRoom);
    const DiffOutcome page = applyTimelineDiff(
        mirror,
        insertDiff(1, itemJson(QStringLiteral("old"), QStringLiteral("$old"),
                               QStringLiteral("old"))),
        kRoom);
    QCOMPARE(page.kind, DiffOutcome::Inserted);
    QVERIFY(!page.liveRemoteInsert);

    QJsonArray echoOnly;
    echoOnly.append(start);
    echoOnly.append(echoItemJson(QStringLiteral("txn1"), QStringLiteral("mine")));
    QList<TimelineEvent> lonely = eventsFromItemArray(echoOnly, kRoom);
    const DiffOutcome first = applyTimelineDiff(
        lonely,
        insertDiff(1, itemJson(QStringLiteral("old"), QStringLiteral("$old"),
                               QStringLiteral("old"))),
        kRoom);
    QVERIFY(!first.liveRemoteInsert);

    QList<TimelineEvent> fresh = eventsFromItemArray(echoOnly, kRoom);
    QJsonObject marked = insertDiff(
        1, itemJson(QStringLiteral("new"), QStringLiteral("$new"), QStringLiteral("hi")));
    marked.insert(QStringLiteral("live_append"), true);
    QVERIFY(applyTimelineDiff(fresh, marked, kRoom).liveRemoteInsert);
    QJsonObject markedEcho =
        insertDiff(1, echoItemJson(QStringLiteral("txn9"), QStringLiteral("x")));
    markedEcho.insert(QStringLiteral("live_append"), true);
    QVERIFY(!applyTimelineDiff(fresh, markedEcho, kRoom).liveRemoteInsert);
}

void RustTimelineIngestTest::aSetReportsDecryptionAndRedactionOfTheSameEventOnly()
{
    QJsonObject utd = itemJson(QStringLiteral("i0"), QStringLiteral("$e"), QString());
    utd.insert(QStringLiteral("undecryptable"), true);
    utd.insert(QStringLiteral("is_encrypted"), true);
    QList<TimelineEvent> mirror = eventsFromItemArray(QJsonArray{ utd }, kRoom);

    QJsonObject setDiff = diffJson(QStringLiteral("set"));
    setDiff.insert(QStringLiteral("index"), 0);
    QJsonObject readable = itemJson(QStringLiteral("i0"), QStringLiteral("$e"),
                                    QStringLiteral("now readable"));
    readable.insert(QStringLiteral("is_encrypted"), true);
    readable.insert(QStringLiteral("is_decrypted"), true);
    setDiff.insert(QStringLiteral("item"), readable);
    const DiffOutcome decrypted = applyTimelineDiff(mirror, setDiff, kRoom);
    QCOMPARE(decrypted.kind, DiffOutcome::Changed);
    QVERIFY2(decrypted.becameDecrypted, "a late decryption was not reported");
    QVERIFY(!decrypted.becameRedacted);

    const DiffOutcome again = applyTimelineDiff(mirror, setDiff, kRoom);
    QVERIFY(!again.becameDecrypted);

    QJsonObject redacted = readable;
    redacted.insert(QStringLiteral("redacted"), true);
    redacted.insert(QStringLiteral("body"), QString());
    setDiff.insert(QStringLiteral("item"), redacted);
    const DiffOutcome gone = applyTimelineDiff(mirror, setDiff, kRoom);
    QVERIFY(gone.becameRedacted);
    QVERIFY(!gone.becameDecrypted);

    QList<TimelineEvent> other = eventsFromItemArray(QJsonArray{ utd }, kRoom);
    QJsonObject swapped = redacted;
    swapped.insert(QStringLiteral("event_id"), QStringLiteral("$other"));
    setDiff.insert(QStringLiteral("item"), swapped);
    const DiffOutcome reused = applyTimelineDiff(other, setDiff, kRoom);
    QVERIFY(!reused.becameDecrypted);
    QVERIFY(!reused.becameRedacted);
}

void RustTimelineIngestTest::theUndecryptableSightingsAreBoundedAndTakenOnce()
{
    matrix::notification_producer::BoundedIdMap seen(2);
    seen.insert(QStringLiteral("$a"), false);
    seen.insert(QStringLiteral("$a"), true);
    seen.insert(QStringLiteral("$b"), false);
    seen.insert(QStringLiteral("$c"), false);
    QCOMPARE(seen.size(), 2);
    QVERIFY(!seen.contains(QStringLiteral("$a")));
    bool backlog = true;
    QVERIFY(seen.take(QStringLiteral("$b"), &backlog));
    QVERIFY(!backlog);
    QVERIFY2(!seen.take(QStringLiteral("$b")), "a decryption was announceable twice");

    matrix::notification_producer::BoundedIdMap flags(4);
    flags.insert(QStringLiteral("$x"), true);
    flags.insert(QStringLiteral("$x"), false);
    QVERIFY(flags.take(QStringLiteral("$x"), &backlog));
    QVERIFY(backlog);
}

void RustTimelineIngestTest::aHighlightRiseNobodyWasToldAboutIsReported()
{
    using matrix::notification_producer::HighlightGapTracker;
    HighlightGapTracker tracker;
    const QString room = QStringLiteral("!r:example.org");
    tracker.observeCount(room, 0, 1000, true);
    tracker.observeCount(room, 2, 5000, true);
    QVERIFY(tracker.takeDue(5000 + HighlightGapTracker::kWindowMs - 1).isEmpty());
    QCOMPARE(tracker.nextDueMs(), 5000 + HighlightGapTracker::kWindowMs);
    const auto due = tracker.takeDue(5000 + HighlightGapTracker::kWindowMs);
    QCOMPARE(due.size(), 1);
    QCOMPARE(due.first().first, room);
    QCOMPARE(due.first().second, 2);
    QVERIFY(tracker.takeDue(99999).isEmpty());
    QCOMPARE(tracker.nextDueMs(), qint64(0));

    tracker.observeCount(room, 5, 20000, true);
    tracker.creditHighlight(room, 20500);
    const auto partly = tracker.takeDue(20000 + HighlightGapTracker::kWindowMs);
    QCOMPARE(partly.size(), 1);
    QCOMPARE(partly.first().second, 2);
}

void RustTimelineIngestTest::aHighlightRiseExplainedByADeliveredEventIsNot()
{
    using matrix::notification_producer::HighlightGapTracker;
    HighlightGapTracker tracker;
    const QString room = QStringLiteral("!r:example.org");
    tracker.observeCount(room, 0, 1000, true);
    tracker.creditHighlight(room, 4000);
    tracker.observeCount(room, 1, 5000, true);
    QVERIFY(tracker.takeDue(10000).isEmpty());
    tracker.observeCount(room, 2, 20000, true);
    tracker.creditHighlight(room, 21000);
    QVERIFY(tracker.takeDue(30000).isEmpty());
    tracker.observeCount(room, 3, 40000, true);
    tracker.observeCount(room, 0, 40500, true);
    QVERIFY(tracker.takeDue(50000).isEmpty());
}

void RustTimelineIngestTest::aHighlightRiseIsNeverReportedForHistoryOrAFirstCount()
{
    using matrix::notification_producer::HighlightGapTracker;
    HighlightGapTracker tracker;
    const QString room = QStringLiteral("!r:example.org");
    tracker.observeCount(room, 4, 1000, true);
    QVERIFY(tracker.takeDue(10000).isEmpty());
    const QString early = QStringLiteral("!early:example.org");
    tracker.observeCount(early, 0, 1000, false);
    tracker.observeCount(early, 3, 2000, false);
    QVERIFY(tracker.takeDue(10000).isEmpty());
    tracker.observeCount(room, 6, 20000, true);
    tracker.noteBacklog(room, 20100);
    QVERIFY(tracker.takeDue(30000).isEmpty());
    tracker.reset();
    tracker.observeCount(room, 9, 40000, true);
    QVERIFY(tracker.takeDue(50000).isEmpty());
    tracker.forgetRoomsNotIn(QSet<QString>{ QStringLiteral("!other:example.org") });
    tracker.observeCount(room, 15, 60000, true);
    QVERIFY(tracker.takeDue(70000).isEmpty());
    tracker.forgetRoom(room);
    tracker.observeCount(room, 20, 80000, true);
    QVERIFY(tracker.takeDue(90000).isEmpty());
    tracker.observeCount(room, 21, 100000, true);
    QCOMPARE(tracker.takeDue(110000).size(), 1);
}

QTEST_GUILESS_MAIN(RustTimelineIngestTest)
#include "RustTimelineIngestTest.moc"
