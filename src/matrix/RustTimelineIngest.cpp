#include "matrix/RustTimelineIngest.h"

#include <QSet>

#include "matrix/EventPreview.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLoggingCategory>
#include <QTimeZone>

#include <algorithm>

// Receipt-movement diagnostics (off by default). Logs counts only, never
// user ids, bodies or event content, to tell a legitimate receipt move from
// one the SDK never delivered.
Q_LOGGING_CATEGORY(lcReceiptDiag, "matrix.receipts", QtWarningMsg)

namespace matrix::rust_timeline {

namespace {

// Matches GALLERY_ITEM_CAP in rust/src/timeline.rs.
constexpr int kMaxGalleryItems = 32;

QDateTime timestampFromMs(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

// Untrusted display names, already capped at 255 code points in Rust; bound
// them here too since this is a pure translator over arbitrary JSON. Counted
// in code points so a cut never splits a surrogate pair, and so a
// bridge-bounded name is never truncated twice.
QString boundedCodePoints(const QString &name, int maxCodePoints)
{
    qsizetype units = 0;
    int points = 0;
    while (units < name.size() && points < maxCodePoints) {
        const bool pair = name.at(units).isHighSurrogate()
            && units + 1 < name.size()
            && name.at(units + 1).isLowSurrogate();
        units += pair ? 2 : 1;
        ++points;
    }
    return units >= name.size() ? name : name.left(units);
}

QString boundedProfileName(const QString &name)
{
    return boundedCodePoints(name, 255);
}

// Attachment names are sender-chosen too; same unit and reason as profile
// names.
QString boundedFilename(const QString &name)
{
    return boundedCodePoints(name, 255);
}

QString boundedMimetype(const QString &mime)
{
    return boundedCodePoints(mime, 127);
}

} // namespace

TimelineEvent::Type rowTypeForMsgtype(const QString &msgtype)
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
    // Calls get their own row kind rather than "state", so they are not folded
    // into the room-updates group.
    if (msgtype == QLatin1String("call"))
        return TimelineEvent::CallEvent;
    // A shared place: rendered with a map link rather than as text.
    if (msgtype == QLatin1String("location"))
        return TimelineEvent::Location;
    if (msgtype == QLatin1String("text"))
        return TimelineEvent::TextMessage;
    if (msgtype == QLatin1String("encrypted") || msgtype == QLatin1String("redacted"))
        return TimelineEvent::Notice;
    return TimelineEvent::Unknown;
}

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
    // Feeds the sanitized-HTML path (MessageHtml::sanitize via
    // FormattedBodyRole); without it rows fall back to the plain body, which
    // for mentions is raw markdown.
    e.formattedBody = item.value(QStringLiteral("formatted_body")).toString();
    e.stateKind = item.value(QStringLiteral("state_kind")).toString();
    e.membershipChange =
        item.value(QStringLiteral("membership_change")).toString();
    e.stateTarget = item.value(QStringLiteral("state_target")).toString();
    // Rust sends null profile_name_change when there was no real rename, which
    // toString() maps to "" (no name change). Absent old/new names stay empty
    // and the sentence builder falls back.
    e.profileNameChange =
        item.value(QStringLiteral("profile_name_change")).toString();
    e.profileNameOld = boundedProfileName(
        item.value(QStringLiteral("profile_name_old")).toString());
    e.profileNameNew = boundedProfileName(
        item.value(QStringLiteral("profile_name_new")).toString());
    e.profileAvatarChanged =
        item.value(QStringLiteral("profile_avatar_changed")).toBool(false);
    // `call_kind` is a closed set: anything else is dropped, because it picks
    // the sentence on a row carrying a Join button. An unknown kind still
    // renders with the generic wording.
    const QString callKind = item.value(QStringLiteral("call_kind")).toString();
    if (callKind == QLatin1String("invite")
        || callKind == QLatin1String("notification"))
        e.callEventKind = callKind;
    e.callIsVideo = item.value(QStringLiteral("call_video")).toBool(false);
    // Clamped at 0; rendered as "N declined".
    e.callDeclinedCount = std::max(
        0, item.value(QStringLiteral("call_declined_count")).toInt(0));
    e.timestamp = timestampFromMs(static_cast<qint64>(
        item.value(QStringLiteral("timestamp_ms")).toDouble(0)));
    if (!e.timestamp.isValid())
        e.timestamp = QDateTime::currentDateTimeUtc();
    e.type = rowTypeForMsgtype(item.value(QStringLiteral("msgtype")).toString());

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
    // Upload progress, present only while the SDK uploads. Absent fields stay
    // 0/0, read as "extent unknown" rather than 0%.
    e.uploadedBytes = static_cast<qint64>(
        item.value(QStringLiteral("send_upload_current")).toDouble(0));
    e.uploadTotalBytes = static_cast<qint64>(
        item.value(QStringLiteral("send_upload_total")).toDouble(0));

    e.replyToEventId = item.value(QStringLiteral("reply_to_event_id")).toString();
    e.replyToSender = item.value(QStringLiteral("reply_to_sender")).toString();
    e.replyToSenderId =
        item.value(QStringLiteral("reply_to_sender_id")).toString();
    // The SDK's reply preview is the plain body, which for a mention carries
    // the matrix.to markdown link verbatim; normalize it like room-list
    // previews. Empty previews skip the regex pass.
    const QString rawReplyPreview =
        item.value(QStringLiteral("reply_to_preview")).toString();
    // The reply quote's own cap: normalizePreviewText's default of 120 would
    // undo Rust's wider budget. The QML label elides to the available width.
    e.replyToPreview =
        rawReplyPreview.isEmpty()
            ? rawReplyPreview
            : matrix::preview::normalizePreviewText(
                  rawReplyPreview, matrix::preview::kReplyPreviewMaxChars);
    e.replyToMediaKey =
        item.value(QStringLiteral("reply_to_media_key")).toString();
    // Closed set like call_kind: this picks the quote's label. The gallery
    // count is clamped to GALLERY_ITEM_CAP so it never exceeds what the row can
    // show.
    static const QStringList kReplyKinds = {
        QStringLiteral("text"),     QStringLiteral("notice"),
        QStringLiteral("emote"),    QStringLiteral("image"),
        QStringLiteral("gif"),      QStringLiteral("video"),
        QStringLiteral("audio"),    QStringLiteral("file"),
        QStringLiteral("sticker"),  QStringLiteral("poll"),
        QStringLiteral("redacted"), QStringLiteral("encrypted"),
    };
    const QString replyKind = item.value(QStringLiteral("reply_to_kind")).toString();
    if (kReplyKinds.contains(replyKind))
        e.replyToKind = replyKind;
    e.replyToCount = std::clamp(
        item.value(QStringLiteral("reply_to_count")).toInt(0), 0,
        kMaxGalleryItems);
    e.threadRootId = item.value(QStringLiteral("thread_root_id")).toString();

    // SDK thread summary on roots; absent fields keep "-1 = unknown" so non-SDK
    // backends count locally.
    e.isThreadRoot = item.value(QStringLiteral("is_thread_root")).toBool(false);
    e.threadReplyCount = item.contains(QStringLiteral("thread_reply_count"))
        ? item.value(QStringLiteral("thread_reply_count")).toInt(-1)
        : -1;
    // Same plain-body source as reply_to_preview; normalize the markdown links.
    const QString rawThreadPreview =
        item.value(QStringLiteral("thread_latest_preview")).toString();
    e.threadLatestPreview = rawThreadPreview.isEmpty()
        ? rawThreadPreview
        : matrix::preview::normalizePreviewText(rawThreadPreview);
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
    e.mediaMimetype =
        boundedMimetype(item.value(QStringLiteral("media_mimetype")).toString());
    const QString rawFilename =
        item.value(QStringLiteral("media_filename")).toString();
    e.mediaFilename = boundedFilename(rawFilename);
    // A body equal to the name must stay equal to the bounded name, or the
    // delegate would show it as a caption (mediaCaptionBody).
    if (e.mediaFilename.size() != rawFilename.size()
        && e.body.trimmed().compare(rawFilename.trimmed(), Qt::CaseInsensitive) == 0)
        e.body = e.mediaFilename;
    e.mediaSize = static_cast<qint64>(
        item.value(QStringLiteral("media_size")).toDouble(0));
    e.mediaWidth = item.value(QStringLiteral("media_width")).toInt(0);
    e.mediaHeight = item.value(QStringLiteral("media_height")).toInt(0);
    e.mediaDurationMs = static_cast<qint64>(
        item.value(QStringLiteral("media_duration_ms")).toDouble(0));
    e.mediaIsVoice = item.value(QStringLiteral("media_voice")).toBool(false);
    const QJsonArray waveform =
        item.value(QStringLiteral("media_waveform")).toArray();
    for (const auto &value : waveform) {
        const int amp = value.toInt(-1);
        if (amp >= 0 && amp <= 100 && e.mediaWaveform.size() < 96)
            e.mediaWaveform.append(amp);
    }

    // Media-bridge key and availability flags; the source itself stays in Rust.
    e.mediaKey = item.value(QStringLiteral("media_key")).toString();
    e.mediaSourceAvailable =
        item.value(QStringLiteral("media_source_available")).toBool(false);
    e.mediaThumbAvailable =
        item.value(QStringLiteral("media_thumb_available")).toBool(false);
    // MSC4274 gallery items, bounded here as well as in Rust. Items without a
    // key or with a kind outside the closed set are dropped; fewer than two
    // survivors is not a gallery, and the row stays a single attachment.
    const QJsonArray galleryItems =
        item.value(QStringLiteral("gallery_items")).toArray();
    for (const auto &value : galleryItems) {
        if (e.galleryItems.size() >= kMaxGalleryItems)
            break;
        const QJsonObject obj = value.toObject();
        GalleryItem g;
        g.mediaKey = obj.value(QStringLiteral("media_key")).toString();
        g.kind = obj.value(QStringLiteral("kind")).toString();
        if (g.mediaKey.isEmpty()
            || !(g.kind == QLatin1String("image")
                 || g.kind == QLatin1String("video")
                 || g.kind == QLatin1String("audio")
                 || g.kind == QLatin1String("file")))
            continue;
        g.filename = boundedFilename(obj.value(QStringLiteral("filename")).toString());
        g.mimetype = boundedMimetype(obj.value(QStringLiteral("mimetype")).toString());
        g.size = std::max<qint64>(0, static_cast<qint64>(
            obj.value(QStringLiteral("size")).toDouble(0)));
        g.width = std::max(0, obj.value(QStringLiteral("width")).toInt(0));
        g.height = std::max(0, obj.value(QStringLiteral("height")).toInt(0));
        g.durationMs = std::max<qint64>(0, static_cast<qint64>(
            obj.value(QStringLiteral("duration_ms")).toDouble(0)));
        g.thumbAvailable =
            obj.value(QStringLiteral("thumb_available")).toBool(false);
        e.galleryItems.append(g);
    }
    if (e.galleryItems.size() < 2)
        e.galleryItems.clear();
    e.senderNameAmbiguous =
        item.value(QStringLiteral("sender_name_ambiguous")).toBool(false);

    const QJsonArray reactions = item.value(QStringLiteral("reactions")).toArray();
    for (const auto &value : reactions) {
        const QJsonObject obj = value.toObject();
        Reaction r;
        r.key = obj.value(QStringLiteral("key")).toString();
        r.count = obj.value(QStringLiteral("count")).toInt(0);
        r.byMe = obj.value(QStringLiteral("by_me")).toBool(false);
        // Reactor ids: a bounded window in SDK order (local user first) beside
        // the uncapped `count`. Non-string or empty entries are dropped. Order
        // is kept so the tooltip can read "You and …" from position 0. Bounded
        // here as well as in Rust; the cap matches REACTION_SENDER_CAP in
        // rust/src/timeline.rs.
        constexpr int kMaxReactionSenders = 16;
        const QJsonArray senders = obj.value(QStringLiteral("senders")).toArray();
        r.senders.reserve(std::min<int>(senders.size(), kMaxReactionSenders));
        for (const auto &sender : senders) {
            if (r.senders.size() >= kMaxReactionSenders)
                break;
            const QString userId = sender.toString();
            if (!userId.isEmpty())
                r.senders.append(userId);
        }
        if (!r.key.isEmpty() && r.count > 0)
            e.reactions.append(r);
    }

    // Read receipts: only the reader's user id and timestamp cross the FFI.
    // Entries without a user id are dropped; a missing ts keeps 0 ("no
    // timestamp"). read_by is a bounded newest-first window (16); read_by_total
    // is the uncapped count, clamped to at least the delivered list so "+N"
    // never undercounts.
    const QJsonArray readBy = item.value(QStringLiteral("read_by")).toArray();
    for (const auto &value : readBy) {
        const QJsonObject obj = value.toObject();
        ReadReceipt receipt;
        receipt.userId = obj.value(QStringLiteral("user_id")).toString();
        receipt.tsMs = static_cast<qint64>(
            obj.value(QStringLiteral("ts")).toDouble(0));
        if (!receipt.userId.isEmpty())
            e.readBy.append(receipt);
    }
    e.readByTotal = qMax(item.value(QStringLiteral("read_by_total")).toInt(0),
                         static_cast<int>(e.readBy.size()));

    // Coordinates are read only when the bridge supplied both. It omits them
    // when the geo URI did not parse or was not on Earth; `contains` separates
    // absent from a legitimate 0.0.
    if (e.type == TimelineEvent::Location) {
        const bool hasLat = item.contains(QStringLiteral("locationLat"));
        const bool hasLon = item.contains(QStringLiteral("locationLon"));
        e.locationHasPoint = hasLat && hasLon;
        if (e.locationHasPoint) {
            e.locationLat = item.value(QStringLiteral("locationLat")).toDouble();
            e.locationLon = item.value(QStringLiteral("locationLon")).toDouble();
        }
        e.locationUncertaintyM =
            item.value(QStringLiteral("locationUncertaintyM")).toDouble(0.0);
        e.locationDescription =
            item.value(QStringLiteral("locationDescription")).toString();
        e.locationAsset = item.value(QStringLiteral("locationAsset")).toString();
        e.locationLive =
            item.value(QStringLiteral("locationLive")).toBool(false);
        e.locationLiveActive =
            item.value(QStringLiteral("locationLiveActive")).toBool(false);
    }

    // MSC3381 poll presentation. Counts arrive pre-gated from Rust (0 for a
    // running undisclosed poll); nothing is re-aggregated here.
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

    // Placeholder for undecryptable rows; the FFI sends an empty body, never
    // ciphertext.
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

    // A snapshot can carry the same message twice: once as a local echo and
    // once as the remote event. On a timeline rebuild matrix-sdk-ui loads
    // remote events first and then pushes still-queued local echoes
    // unconditionally (the dedup in recycle_local_or_create_item only runs on
    // the remote path). The queue can still owe an echo because matrix-sdk
    // disables a room's send queue after a send error (see
    // rust/src/timeline.rs).
    //
    // Matched on event id, not transaction id: transaction_id() is None for
    // every remote item, while event_id() is set on both once the send request
    // returned. Two items sharing an event id are the same message. The remote
    // row wins; an echo without an event id is genuinely pending and kept.
    QSet<QString> remoteIds;
    for (const auto &e : std::as_const(out)) {
        if (!e.isLocalEcho && !e.eventId.isEmpty())
            remoteIds.insert(e.eventId);
    }
    if (!remoteIds.isEmpty()) {
        out.removeIf([&remoteIds](const TimelineEvent &e) {
            return e.isLocalEcho && !e.eventId.isEmpty()
                   && remoteIds.contains(e.eventId);
        });
    }
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
        // An empty append is a no-op, not a corruption signal.
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
        if (Q_UNLIKELY(lcReceiptDiag().isDebugEnabled())) {
            const int before = mirror.at(index).readBy.size();
            const int after = out.items.first().readBy.size();
            if (before != after) {
                qCDebug(lcReceiptDiag)
                    << "set moved receipts index=" << index
                    << "before=" << before << "after=" << after
                    << "totalBefore=" << mirror.at(index).readByTotal
                    << "totalAfter=" << out.items.first().readByTotal;
            }
        }
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

QHash<QString, MemberInfo> membersFromPayload(const QJsonArray &rows)
{
    QHash<QString, MemberInfo> members;
    members.reserve(rows.size());
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const QString userId = row.value(QStringLiteral("user_id")).toString();
        if (userId.isEmpty())
            continue;
        MemberInfo info;
        info.userId = userId;
        info.displayName =
            row.value(QStringLiteral("display_name")).toString();
        info.avatarMxcUrl =
            row.value(QStringLiteral("avatar_url")).toString();
        members.insert(userId, info);
    }
    return members;
}

} // namespace matrix::rust_timeline
