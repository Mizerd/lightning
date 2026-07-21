#include "matrix/RustTimelineIngest.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>

namespace matrix::rust_timeline {

namespace {

QDateTime timestampFromMs(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

TimelineEvent::Type messageType(const QString &msgtype)
{
    if (msgtype == QLatin1String("notice"))
        return TimelineEvent::Notice;
    if (msgtype == QLatin1String("emote"))
        return TimelineEvent::Emote;
    if (msgtype == QLatin1String("image"))
        return TimelineEvent::Image;
    if (msgtype == QLatin1String("file"))
        return TimelineEvent::File;
    if (msgtype == QLatin1String("video"))
        return TimelineEvent::Video;
    if (msgtype == QLatin1String("audio"))
        return TimelineEvent::Audio;
    if (msgtype == QLatin1String("sticker"))
        return TimelineEvent::Sticker;
    if (msgtype == QLatin1String("poll"))
        return TimelineEvent::Poll;
    if (msgtype == QLatin1String("state"))
        return TimelineEvent::StateChange;
    if (msgtype == QLatin1String("text"))
        return TimelineEvent::TextMessage;
    if (msgtype == QLatin1String("encrypted") || msgtype == QLatin1String("redacted"))
        return TimelineEvent::Notice;
    return TimelineEvent::Unknown;
}

} // namespace

TimelineEvent eventFromItemJson(const QJsonObject &item, const QString &roomId)
{
    TimelineEvent e;
    e.roomId = roomId;
    e.itemId = item.value(QStringLiteral("item_id")).toString();

    const QString kind = item.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("date_divider")) {
        e.type = TimelineEvent::DateDivider;
        e.timestamp = timestampFromMs(static_cast<qint64>(
            item.value(QStringLiteral("timestamp_ms")).toDouble(0)));
        e.eventId.clear();
        return e;
    }
    if (kind == QLatin1String("read_marker")) {
        e.type = TimelineEvent::ReadMarker;
        return e;
    }
    if (kind == QLatin1String("timeline_start")) {
        e.type = TimelineEvent::TimelineStart;
        return e;
    }

    e.eventId = item.value(QStringLiteral("event_id")).toString();
    e.transactionId = item.value(QStringLiteral("transaction_id")).toString();
    e.sender = item.value(QStringLiteral("sender")).toString();
    e.senderDisplayName =
        item.value(QStringLiteral("sender_display_name")).toString();
    e.senderAvatarUrl =
        item.value(QStringLiteral("sender_avatar_url")).toString();
    e.body = item.value(QStringLiteral("body")).toString();
    e.stateKind = item.value(QStringLiteral("state_kind")).toString();
    e.stateTarget = item.value(QStringLiteral("state_target")).toString();
    e.timestamp = timestampFromMs(static_cast<qint64>(
        item.value(QStringLiteral("timestamp_ms")).toDouble(0)));
    if (!e.timestamp.isValid())
        e.timestamp = QDateTime::currentDateTimeUtc();
    e.type = messageType(item.value(QStringLiteral("msgtype")).toString());

    e.edited = item.value(QStringLiteral("edited")).toBool(false);
    e.redacted = item.value(QStringLiteral("redacted")).toBool(false);
    e.isEncrypted = item.value(QStringLiteral("is_encrypted")).toBool(false);
    e.isDecrypted = item.value(QStringLiteral("is_decrypted")).toBool(false);
    e.undecryptable = item.value(QStringLiteral("undecryptable")).toBool(false);
    e.errorKind = item.value(QStringLiteral("error_kind")).toString();
    e.isLocalEcho = item.value(QStringLiteral("is_local_echo")).toBool(false);

    const QString sendState = item.value(QStringLiteral("send_state")).toString();
    if (sendState == QLatin1String("sending"))
        e.status = TimelineEvent::Sending;
    else if (sendState == QLatin1String("failed"))
        e.status = TimelineEvent::Failed;
    else
        e.status = TimelineEvent::Sent;
    e.sendErrorCategory = item.value(QStringLiteral("send_error")).toString();

    e.replyToEventId = item.value(QStringLiteral("reply_to_event_id")).toString();
    e.replyToSender = item.value(QStringLiteral("reply_to_sender")).toString();
    e.replyToPreview = item.value(QStringLiteral("reply_to_preview")).toString();
    e.threadRootId = item.value(QStringLiteral("thread_root_id")).toString();

    // v0.6.0: SDK thread summary on thread root events (absent fields keep
    // the "-1 = unknown" contract so non-SDK backends fall back to local
    // counting).
    e.isThreadRoot = item.value(QStringLiteral("is_thread_root")).toBool(false);
    e.threadReplyCount = item.contains(QStringLiteral("thread_reply_count"))
        ? item.value(QStringLiteral("thread_reply_count")).toInt(-1)
        : -1;
    e.threadLatestPreview =
        item.value(QStringLiteral("thread_latest_preview")).toString();
    e.threadLatestKind =
        item.value(QStringLiteral("thread_latest_kind")).toString();
    e.threadLatestSender =
        item.value(QStringLiteral("thread_latest_sender")).toString();
    e.threadLatestSenderDisplayName =
        item.value(QStringLiteral("thread_latest_sender_display_name")).toString();
    e.threadLatestSenderAvatarUrl =
        item.value(QStringLiteral("thread_latest_sender_avatar_url")).toString();
    const auto latestTs = static_cast<qint64>(
        item.value(QStringLiteral("thread_latest_timestamp_ms")).toDouble(0));
    if (latestTs > 0)
        e.threadLatestTimestamp =
            QDateTime::fromMSecsSinceEpoch(latestTs, Qt::UTC);
    e.threadUnread = item.value(QStringLiteral("thread_unread")).toBool(false);
    e.mentionsMe = item.value(QStringLiteral("mentions_me")).toBool(false);
    e.mentionsRoom = item.value(QStringLiteral("mentions_room")).toBool(false);

    e.mediaMxcUrl = item.value(QStringLiteral("media_mxc")).toString();
    e.mediaMimetype = item.value(QStringLiteral("media_mimetype")).toString();
    e.mediaFilename = item.value(QStringLiteral("media_filename")).toString();
    e.mediaSize = static_cast<qint64>(
        item.value(QStringLiteral("media_size")).toDouble(0));
    e.mediaWidth = item.value(QStringLiteral("media_width")).toInt(0);
    e.mediaHeight = item.value(QStringLiteral("media_height")).toInt(0);
    e.mediaDurationMs = static_cast<qint64>(
        item.value(QStringLiteral("media_duration_ms")).toDouble(0));
    e.mediaIsVoice = item.value(QStringLiteral("media_voice")).toBool(false);

    // v0.5.9: media-bridge retrieval key + availability flags (set for both
    // plain and encrypted sources; the source itself stays inside Rust).
    e.mediaKey = item.value(QStringLiteral("media_key")).toString();
    e.mediaSourceAvailable =
        item.value(QStringLiteral("media_source_available")).toBool(false);
    e.mediaThumbAvailable =
        item.value(QStringLiteral("media_thumb_available")).toBool(false);
    e.senderNameAmbiguous =
        item.value(QStringLiteral("sender_name_ambiguous")).toBool(false);

    const QJsonArray reactions = item.value(QStringLiteral("reactions")).toArray();
    for (const auto &value : reactions) {
        const QJsonObject obj = value.toObject();
        Reaction r;
        r.key = obj.value(QStringLiteral("key")).toString();
        r.count = obj.value(QStringLiteral("count")).toInt(0);
        r.byMe = obj.value(QStringLiteral("by_me")).toBool(false);
        if (!r.key.isEmpty() && r.count > 0)
            e.reactions.append(r);
    }

    // v0.7: MSC3381 poll presentation. Counts arrive pre-gated from Rust
    // (0 for a running undisclosed poll); nothing here re-aggregates.
    if (e.type == TimelineEvent::Poll) {
        e.pollQuestion = item.value(QStringLiteral("poll_question")).toString();
        e.pollKind = item.value(QStringLiteral("poll_kind")).toString();
        e.pollMaxSelections =
            item.value(QStringLiteral("poll_max_selections")).toInt(1);
        e.pollTotalVoters =
            item.value(QStringLiteral("poll_total_voters")).toInt(0);
        e.pollEnded = item.value(QStringLiteral("poll_ended")).toBool(false);
        const QJsonArray answers =
            item.value(QStringLiteral("poll_answers")).toArray();
        for (const auto &value : answers) {
            const QJsonObject obj = value.toObject();
            PollAnswer answer;
            answer.id = obj.value(QStringLiteral("id")).toString();
            answer.text = obj.value(QStringLiteral("text")).toString();
            answer.count = obj.value(QStringLiteral("count")).toInt(0);
            answer.byMe = obj.value(QStringLiteral("by_me")).toBool(false);
            if (!answer.id.isEmpty())
                e.pollAnswers.append(answer);
        }
    }

    // Honest placeholder for undecryptable rows — the FFI contract sends an
    // empty body and never ciphertext.
    if (e.undecryptable && e.body.isEmpty()) {
        e.body = QCoreApplication::translate("RustTimeline",
                                             "[unable to decrypt yet]");
        e.type = TimelineEvent::Notice;
    }

    return e;
}

QList<TimelineEvent> eventsFromItemArray(const QJsonArray &items,
                                         const QString &roomId)
{
    QList<TimelineEvent> out;
    out.reserve(items.size());
    for (const auto &value : items)
        out.append(eventFromItemJson(value.toObject(), roomId));
    return out;
}

DiffOutcome applyTimelineDiff(QList<TimelineEvent> &mirror,
                              const QJsonObject &diff,
                              const QString &roomId)
{
    DiffOutcome out;
    const QString op = diff.value(QStringLiteral("op")).toString();

    const auto singleItem = [&]() -> TimelineEvent {
        return eventFromItemJson(diff.value(QStringLiteral("item")).toObject(),
                                 roomId);
    };

    if (op == QLatin1String("append")) {
        const QJsonArray items = diff.value(QStringLiteral("items")).toArray();
        out.items = eventsFromItemArray(items, roomId);
        // An empty append is a harmless no-op, not a corruption signal.
        mirror.append(out.items);
        out.kind = DiffOutcome::Appended;
        return out;
    }
    if (op == QLatin1String("push_back")) {
        if (!diff.contains(QStringLiteral("item")))
            return out;
        out.items.append(singleItem());
        mirror.append(out.items.first());
        out.kind = DiffOutcome::Appended;
        return out;
    }
    if (op == QLatin1String("push_front")) {
        if (!diff.contains(QStringLiteral("item")))
            return out;
        out.items.append(singleItem());
        mirror.prepend(out.items.first());
        out.kind = DiffOutcome::Prepended;
        return out;
    }
    if (op == QLatin1String("insert")) {
        const int index = diff.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index > mirror.size()
            || !diff.contains(QStringLiteral("item")))
            return out;
        out.items.append(singleItem());
        mirror.insert(index, out.items.first());
        out.kind = DiffOutcome::Inserted;
        out.index = index;
        return out;
    }
    if (op == QLatin1String("set")) {
        const int index = diff.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= mirror.size()
            || !diff.contains(QStringLiteral("item")))
            return out;
        out.items.append(singleItem());
        mirror[index] = out.items.first();
        out.kind = DiffOutcome::Changed;
        out.index = index;
        return out;
    }
    if (op == QLatin1String("remove")) {
        const int index = diff.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= mirror.size())
            return out;
        mirror.removeAt(index);
        out.kind = DiffOutcome::Removed;
        out.index = index;
        return out;
    }
    if (op == QLatin1String("pop_front")) {
        if (mirror.isEmpty())
            return out;
        mirror.removeFirst();
        out.kind = DiffOutcome::Removed;
        out.index = 0;
        return out;
    }
    if (op == QLatin1String("pop_back")) {
        if (mirror.isEmpty())
            return out;
        out.index = mirror.size() - 1;
        mirror.removeLast();
        out.kind = DiffOutcome::Removed;
        return out;
    }
    if (op == QLatin1String("clear")) {
        mirror.clear();
        out.kind = DiffOutcome::Cleared;
        return out;
    }
    if (op == QLatin1String("truncate")) {
        const int length = diff.value(QStringLiteral("length")).toInt(-1);
        if (length < 0 || length > mirror.size())
            return out;
        while (mirror.size() > length)
            mirror.removeLast();
        out.kind = DiffOutcome::Truncated;
        out.length = length;
        return out;
    }
    if (op == QLatin1String("reset")) {
        const QJsonArray items = diff.value(QStringLiteral("items")).toArray();
        out.items = eventsFromItemArray(items, roomId);
        mirror = out.items;
        out.kind = DiffOutcome::Reset;
        return out;
    }

    return out; // unknown op → Invalid, mirror untouched
}

} // namespace matrix::rust_timeline
