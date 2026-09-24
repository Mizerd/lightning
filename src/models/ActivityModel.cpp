#include "models/ActivityModel.h"

#include "matrix/MatrixClient.h"

#include <QDateTime>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace {
// Every kind a row may claim. "highlight" is what the server gives without
// saying which push rule matched, so it claims only that.
const QStringList kKnownKinds{
    QStringLiteral("mention"),      QStringLiteral("room_mention"),
    QStringLiteral("reply"),        QStringLiteral("thread"),
    QStringLiteral("reaction"),     QStringLiteral("invite"),
    QStringLiteral("keyword"),      QStringLiteral("highlight"),
};
constexpr int kMaxOwnEventIds = 2048;

QString collapse(const QString &text)
{
    QString out = text.simplified();
    if (out.size() > ActivityModel::kPreviewChars)
        out = out.left(ActivityModel::kPreviewChars - 1) + QChar(0x2026);
    return out;
}
} // namespace

ActivityModel::ActivityModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void ActivityModel::setStore(Store store)
{
    m_store = std::move(store);
    m_storeLoaded = false;
}

void ActivityModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clear();
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::loggedOut, this, [this] { clear(); });
    connect(m_client, &MatrixClient::connectionStateChanged, this,
            [this](MatrixClient::ConnectionState state) {
        if (state == MatrixClient::Syncing)
            loadStore();
    });
    // A room whose unread state has gone clear has been read, on this device or
    // another; see reconcileRoomsAgainstTheirReadState().
    //
    // roomsChanged, not roomUpdated: the unread fields are only written by the
    // room payload handlers, which emit roomsChanged. roomUpdated comes from
    // the timeline path, which raises lastActivity to the new event without
    // touching the counters, so listening to it would mark a brand-new mention
    // seen the instant it arrived.
    connect(m_client, &MatrixClient::roomsChanged, this,
            &ActivityModel::reconcileRoomsAgainstTheirReadState);
    // The payload that carries the unread counters also carries the room's
    // name.
    connect(m_client, &MatrixClient::roomsChanged, this, [this] {
        resolvePendingSeedNames();
        if (m_seedAwaitingRooms.isEmpty())
            return;
        const QStringList pending = std::move(m_seedAwaitingRooms);
        bool flipped = false;
        m_seedAwaitingRooms =
            reconcileSeedAgainstRoomCounts(pending, &flipped);
        // Only when a row changed, and as dataChanged rather than a reset:
        // seenMark does not affect passesFilter(), so no row's visibility
        // moves, and a reset would lose scroll position and delegate state (as
        // in markAllSeen()).
        if (flipped) {
            if (!m_visible.isEmpty())
                Q_EMIT dataChanged(index(0), index(m_visible.size() - 1),
                                   { SeenRole });
            // Outside the visible-rows guard: unseenCount() counts every entry,
            // not just visible rows.
            Q_EMIT unseenCountChanged();
        }
    });
    // A member snapshot turns a sender id into a name. membersChanged, not
    // roomMemberEventSeen, which fires per member event and is meant for roster
    // refetches only. It is emitted only for full rosters of rooms something
    // fetched; the roomsChanged sweep above re-resolves everything else.
    connect(m_client, &MatrixClient::membersChanged, this,
            [this](const QString &roomId) { resolvePendingSeedNames(roomId); });
}

void ActivityModel::loadStore()
{
    if (m_storeLoaded || !m_store.load)
        return;
    m_storeLoaded = true;
    const QVariantMap saved = m_store.load();
    m_seenUpToMs = saved.value(QStringLiteral("seenUpToMs")).toLongLong();
    QStringList kws;
    for (const QString &k : saved.value(QStringLiteral("keywords")).toStringList()) {
        const QString t = k.trimmed().left(kMaxKeywordLength);
        if (!t.isEmpty() && !kws.contains(t, Qt::CaseInsensitive))
            kws.append(t);
        if (kws.size() >= kMaxKeywords)
            break;
    }
    if (kws != m_keywords) {
        m_keywords = kws;
        Q_EMIT keywordsChanged();
    }
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

void ActivityModel::saveStore()
{
    if (!m_store.save)
        return;
    m_store.save(QVariantMap{
        { QStringLiteral("seenUpToMs"), m_seenUpToMs },
        { QStringLiteral("keywords"), m_keywords },
    });
}

void ActivityModel::clear()
{
    beginResetModel();
    m_entries.clear();
    m_visible.clear();
    m_ids.clear();
    m_ownEventIds.clear();
    m_ownEventOrder.clear();
    m_ownThreadRoots.clear();
    m_seedAwaitingRooms.clear();
    m_seenUpToMs = 0;
    m_storeLoaded = false;
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT unseenCountChanged();
}

// ---- roles ---------------------------------------------------------------

int ActivityModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visible.size();
}

QHash<int, QByteArray> ActivityModel::roleNames() const
{
    return {
        { IdRole, "entryId" },
        { KindRole, "kind" },
        { RoomIdRole, "roomId" },
        { RoomNameRole, "roomName" },
        { SenderIdRole, "senderId" },
        { SenderNameRole, "senderName" },
        { PreviewRole, "preview" },
        { TimestampMsRole, "timestampMs" },
        { EventIdRole, "eventId" },
        { ThreadRootIdRole, "threadRootId" },
        { SeenRole, "seen" },
        { EncryptedRole, "encrypted" },
        { ReactionKeyRole, "reactionKey" },
    };
}

QVariant ActivityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const Entry &e = m_entries.at(m_visible.at(index.row()));
    switch (role) {
    case IdRole: return e.id;
    case KindRole: return e.kind;
    case RoomIdRole: return e.roomId;
    case RoomNameRole: return e.roomName;
    case SenderIdRole: return e.senderId;
    case SenderNameRole: return e.senderName;
    case PreviewRole: return e.preview;
    case TimestampMsRole: return e.timestampMs;
    case EventIdRole: return e.eventId;
    case ThreadRootIdRole: return e.threadRootId;
    case SeenRole: return isSeen(e);
    case EncryptedRole: return e.encrypted;
    case ReactionKeyRole: return e.reactionKey;
    }
    return {};
}

QVariantMap ActivityModel::entryAt(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= m_visible.size())
        return out;
    const QModelIndex idx = index(row);
    const auto names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        out.insert(QString::fromLatin1(it.value()), data(idx, it.key()));
    return out;
}

// ---- seen state ----------------------------------------------------------

bool ActivityModel::isSeen(const Entry &e) const
{
    return e.seenMark || (e.timestampMs > 0 && e.timestampMs <= m_seenUpToMs);
}

int ActivityModel::unseenCount() const
{
    int n = 0;
    for (const Entry &e : m_entries)
        if (!isSeen(e))
            ++n;
    return n;
}

void ActivityModel::markAllSeen()
{
    qint64 newest = m_seenUpToMs;
    for (Entry &e : m_entries) {
        e.seenMark = true;
        newest = std::max(newest, e.timestampMs);
    }
    // Never mark past "now": a future-dated (skewed) entry stays unseen.
    m_seenUpToMs = std::min(newest, QDateTime::currentMSecsSinceEpoch());
    saveStore();
    if (!m_visible.isEmpty())
        Q_EMIT dataChanged(index(0), index(m_visible.size() - 1), { SeenRole });
    Q_EMIT unseenCountChanged();
}

void ActivityModel::markRoomReadUpTo(const QString &roomId, qint64 timestampMs)
{
    if (roomId.isEmpty() || timestampMs <= 0)
        return;
    bool changed = false;
    for (Entry &e : m_entries) {
        if (e.seenMark || e.roomId != roomId)
            continue;
        // An entry without a timestamp cannot be compared; leave it rather than
        // assume it is old.
        if (e.timestampMs <= 0 || e.timestampMs > timestampMs)
            continue;
        e.seenMark = true;
        changed = true;
    }
    if (!changed)
        return;
    // Per-entry marks only: a receipt in one room says nothing about other
    // rooms, so m_seenUpToMs stays put. Not persisted; across a restart
    // reconcileSeedAgainstRoomCounts() re-derives it from the server, since the
    // per-notification `read` flag alone was found to be unreliable.
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

// One pass over the rooms holding unseen rows, so a batch costs one rebuild.
void ActivityModel::reconcileRoomsAgainstTheirReadState()
{
    if (!m_client || !m_client->tracksRoomReadState())
        return;
    QSet<QString> rooms;
    for (const Entry &e : m_entries) {
        // isSeen(), not seenMark: after a restart the marker carries the state
        // and seenMark is false everywhere.
        if (!isSeen(e) && e.kind != QLatin1String("invite"))
            rooms.insert(e.roomId);
    }
    bool changed = false;
    for (const QString &id : std::as_const(rooms))
        changed = markRoomReadIfClear(id) || changed;
    if (!changed)
        return;
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

// The marking half, without signalling; the batch above emits once.
bool ActivityModel::markRoomReadIfClear(const QString &roomId)
{
    if (!m_client || roomId.isEmpty())
        return false;
    // A backend that does not track read state must not be read as reporting
    // "read": the mock and HTTP backends never write hasUnreadMessages or
    // markedUnread, leaving only notification_count, which is not a read
    // signal. See MatrixClient::tracksRoomReadState.
    if (!m_client->tracksRoomReadState())
        return false;
    // Cheap rejection first; this runs on every room payload.
    bool holds = false;
    for (const Entry &e : m_entries) {
        if (!isSeen(e) && e.roomId == roomId
            && e.kind != QLatin1String("invite")) {
            holds = true;
            break;
        }
    }
    if (!holds)
        return false;
    const RoomInfo info = m_client->roomInfo(roomId);
    if (info.id != roomId)
        return false;
    // All of them must be clear. num_unread_messages is receipt-derived and
    // works across devices; the counts and the manual flag keep a
    // partially-read room's rows.
    if (info.markedUnread || info.hasUnreadMessages || info.unreadCount > 0
        || info.highlightCount > 0) {
        return false;
    }
    // The room's newest known activity bounds how far the user can have read.
    if (!info.lastActivity.isValid())
        return false;
    const qint64 readUpToMs = info.lastActivity.toMSecsSinceEpoch();
    if (readUpToMs <= 0)
        return false;
    // Per-entry marks only, as in markRoomReadUpTo; m_seenUpToMs does not move.
    // Durability across a restart comes from seed() and
    // reconcileSeedAgainstRoomCounts().
    bool changed = false;
    for (Entry &e : m_entries) {
        if (e.seenMark || e.roomId != roomId)
            continue;
        if (e.kind == QLatin1String("invite"))
            continue;
        if (e.timestampMs <= 0 || e.timestampMs > readUpToMs)
            continue;
        e.seenMark = true;
        changed = true;
    }
    return changed;
}

void ActivityModel::markSeen(const QString &id)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.id != id)
            continue;
        if (e.seenMark)
            return;
        e.seenMark = true;
        const int row = m_visible.indexOf(i);
        if (row >= 0)
            Q_EMIT dataChanged(index(row), index(row), { SeenRole });
        Q_EMIT unseenCountChanged();
        return;
    }
}

void ActivityModel::open(const QString &id)
{
    for (const Entry &e : m_entries) {
        if (e.id != id)
            continue;
        markSeen(id);
        Q_EMIT openRequested(e.roomId, e.eventId, e.threadRootId);
        return;
    }
}

// ---- filter --------------------------------------------------------------

void ActivityModel::setFilter(const QString &filter)
{
    static const QStringList kFilters{
        QStringLiteral("all"), QStringLiteral("mentions"), QStringLiteral("replies"),
        QStringLiteral("threads"), QStringLiteral("reactions"),
        QStringLiteral("invites"), QStringLiteral("keywords"),
    };
    const QString next = kFilters.contains(filter) ? filter : QStringLiteral("all");
    if (next == m_filter)
        return;
    m_filter = next;
    rebuildVisible();
    Q_EMIT filterChanged();
}

bool ActivityModel::passesFilter(const Entry &e) const
{
    if (m_filter == QLatin1String("all"))
        return true;
    if (m_filter == QLatin1String("mentions"))
        return e.kind == QLatin1String("mention") || e.kind == QLatin1String("room_mention");
    if (m_filter == QLatin1String("replies"))
        return e.kind == QLatin1String("reply");
    if (m_filter == QLatin1String("threads"))
        return e.kind == QLatin1String("thread");
    if (m_filter == QLatin1String("reactions"))
        return e.kind == QLatin1String("reaction");
    if (m_filter == QLatin1String("invites"))
        return e.kind == QLatin1String("invite");
    if (m_filter == QLatin1String("keywords"))
        return e.kind == QLatin1String("keyword");
    return true;
}

void ActivityModel::rebuildVisible()
{
    beginResetModel();
    m_visible.clear();
    for (int i = 0; i < m_entries.size(); ++i)
        if (passesFilter(m_entries.at(i)))
            m_visible.append(i);
    endResetModel();
    Q_EMIT countChanged();
}

// ---- keywords ------------------------------------------------------------

void ActivityModel::setKeywords(const QStringList &keywords)
{
    QStringList next;
    for (const QString &k : keywords) {
        const QString t = k.trimmed().left(kMaxKeywordLength);
        if (t.isEmpty() || next.contains(t, Qt::CaseInsensitive))
            continue;
        next.append(t);
        if (next.size() >= kMaxKeywords)
            break;
    }
    if (next == m_keywords)
        return;
    m_keywords = next;
    saveStore();
    Q_EMIT keywordsChanged();
}

bool ActivityModel::matchesKeyword(const QString &body, const QString &keyword)
{
    const QString k = keyword.trimmed();
    if (k.isEmpty() || body.isEmpty())
        return false;
    // Whole-word, case-insensitive. A keyword starting or ending with
    // punctuation (e.g. "#lightning") needs a boundary only on its
    // word-character side.
    QString pattern;
    if (k.front().isLetterOrNumber())
        pattern += QStringLiteral("(?<![\\p{L}\\p{N}_])");
    pattern += QRegularExpression::escape(k);
    if (k.back().isLetterOrNumber())
        pattern += QStringLiteral("(?![\\p{L}\\p{N}_])");
    const QRegularExpression re(pattern,
                                QRegularExpression::CaseInsensitiveOption
                                    | QRegularExpression::UseUnicodePropertiesOption);
    return re.match(body).hasMatch();
}

// ---- classification ------------------------------------------------------

QString ActivityModel::classify(const TimelineEvent &event, const QString &selfUserId,
                                const QSet<QString> &ownEventIds,
                                const QSet<QString> &ownThreadRoots,
                                const QStringList &keywords)
{
    if (event.isVirtual() || event.isLocalEcho || event.redacted)
        return {};
    if (event.type == TimelineEvent::StateChange || event.type == TimelineEvent::CallEvent
        || event.type == TimelineEvent::Unknown)
        return {};
    if (event.sender.isEmpty() || event.sender == selfUserId)
        return {};
    if (event.mentionsMe)
        return QStringLiteral("mention");
    if ((!event.replyToSenderId.isEmpty() && event.replyToSenderId == selfUserId)
        || (!event.replyToEventId.isEmpty() && ownEventIds.contains(event.replyToEventId)))
        return QStringLiteral("reply");
    if (!event.threadRootId.isEmpty() && ownThreadRoots.contains(event.threadRootId))
        return QStringLiteral("thread");
    if (!event.undecryptable) {
        for (const QString &k : keywords)
            if (matchesKeyword(event.body, k))
                return QStringLiteral("keyword");
    }
    if (event.mentionsRoom)
        return QStringLiteral("room_mention");
    return {};
}

QString ActivityModel::previewOf(const TimelineEvent &event)
{
    if (event.undecryptable)
        return {};
    switch (event.type) {
    case TimelineEvent::Image: return QStringLiteral("📷");
    case TimelineEvent::Video: return QStringLiteral("🎬");
    case TimelineEvent::Audio: return QStringLiteral("🎵");
    case TimelineEvent::File: return QStringLiteral("📎");
    case TimelineEvent::Sticker: return QStringLiteral("🩷");
    case TimelineEvent::Poll: return collapse(event.pollQuestion);
    default: break;
    }
    return collapse(event.body);
}

void ActivityModel::rememberOwn(const TimelineEvent &event)
{
    if (event.eventId.isEmpty() || m_ownEventIds.contains(event.eventId))
        return;
    m_ownEventIds.insert(event.eventId);
    m_ownEventOrder.append(event.eventId);
    while (m_ownEventOrder.size() > kMaxOwnEventIds) {
        m_ownEventIds.remove(m_ownEventOrder.first());
        m_ownEventOrder.removeFirst();
    }
    // A thread the user started or replied in is "their" thread.
    if (event.isThreadRoot)
        m_ownThreadRoots.insert(event.eventId);
    if (!event.threadRootId.isEmpty())
        m_ownThreadRoots.insert(event.threadRootId);
}

bool ActivityModel::ingest(const TimelineEvent &event, const QString &roomName)
{
    if (!m_client)
        return false;
    const QString self = m_client->currentUserId();
    if (event.sender == self && !event.isLocalEcho && !event.isVirtual()) {
        rememberOwn(event);
        return false;
    }
    const QString kind = classify(event, self, m_ownEventIds, m_ownThreadRoots, m_keywords);
    if (kind.isEmpty() || event.eventId.isEmpty())
        return false;
    Entry e;
    e.id = event.eventId;
    e.kind = kind;
    e.roomId = event.roomId.isEmpty() ? QString() : event.roomId;
    e.roomName = roomName;
    e.roomNamePending = roomName.isEmpty() || roomName == e.roomId;
    e.senderId = event.sender;
    e.senderName = event.senderDisplayName.isEmpty() ? event.sender
                                                     : event.senderDisplayName;
    // As in the seed: a live highlight can arrive before the room's roster, and
    // an id is not a name. The resolver works off these flags.
    e.senderNamePending = e.senderName == e.senderId;
    e.preview = previewOf(event);
    e.encrypted = event.undecryptable;
    e.timestampMs = event.timestamp.isValid() ? event.timestamp.toMSecsSinceEpoch() : 0;
    e.eventId = event.eventId;
    e.threadRootId = event.threadRootId;
    return addEntry(std::move(e));
}

bool ActivityModel::noteReaction(const QString &roomId, const QString &roomName,
                                 const QString &reactionEventId,
                                 const QString &targetEventId, const QString &senderId,
                                 const QString &senderName, const QString &key,
                                 qint64 timestampMs)
{
    if (!m_client || reactionEventId.isEmpty() || targetEventId.isEmpty())
        return false;
    if (senderId.isEmpty() || senderId == m_client->currentUserId())
        return false;
    if (!m_ownEventIds.contains(targetEventId))
        return false;
    Entry e;
    e.id = reactionEventId;
    e.kind = QStringLiteral("reaction");
    e.roomId = roomId;
    e.roomName = roomName;
    e.roomNamePending = roomName.isEmpty() || roomName == e.roomId;
    e.senderId = senderId;
    e.senderName = senderName.isEmpty() ? senderId : senderName;
    e.senderNamePending = e.senderName == e.senderId;
    e.reactionKey = key.left(32);
    e.preview = e.reactionKey;
    e.timestampMs = timestampMs;
    e.eventId = targetEventId;   // navigate to the message that was reacted to
    return addEntry(std::move(e));
}

bool ActivityModel::noteInvite(const RoomInfo &room)
{
    if (room.id.isEmpty() || room.membership != RoomInfo::Invited)
        return false;
    Entry e;
    e.id = QStringLiteral("invite:") + room.id;
    if (m_ids.contains(e.id))
        return false;
    e.kind = QStringLiteral("invite");
    e.roomId = room.id;
    e.roomName = room.name.isEmpty() ? room.id : room.name;
    e.roomNamePending = room.name.isEmpty();
    e.senderId = room.inviterUserId;
    e.senderName = room.inviterDisplayName.isEmpty() ? room.inviterUserId
                                                     : room.inviterDisplayName;
    // Invites are where a raw id is most likely to stick (no roster is fetched
    // for a room the user is not in); the roomsChanged sweep can still name the
    // room from the invite payload.
    e.senderNamePending = e.senderName == e.senderId;
    e.timestampMs = QDateTime::currentMSecsSinceEpoch();
    return addEntry(std::move(e));
}

void ActivityModel::inviteResolved(const QString &roomId)
{
    const QString id = QStringLiteral("invite:") + roomId;
    if (!m_ids.contains(id))
        return;
    m_ids.remove(id);
    m_entries.removeIf([&id](const Entry &e) { return e.id == id; });
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

void ActivityModel::seed(const QVariantList &entries)
{
    if (!m_client)
        return;
    const QString self = m_client->currentUserId();
    bool any = false;
    QStringList seeded;
    for (const QVariant &v : entries) {
        const QVariantMap m = v.toMap();
        Entry e;
        e.id = m.value(QStringLiteral("eventId")).toString();
        e.roomId = m.value(QStringLiteral("roomId")).toString();
        e.senderId = m.value(QStringLiteral("senderId")).toString();
        if (e.id.isEmpty() || e.roomId.isEmpty() || m_ids.contains(e.id)
            || e.senderId == self)
            continue;
        // Validated against the known set, like setFilter: an unknown kind
        // would render with no label. "highlight" is the honest default for
        // server highlights.
        e.kind = m.value(QStringLiteral("kind")).toString();
        if (!kKnownKinds.contains(e.kind))
            e.kind = QStringLiteral("highlight");
        e.roomName = m.value(QStringLiteral("roomName")).toString();
        if (e.roomName.isEmpty() && m_client)
            e.roomName = m_client->roomInfo(e.roomId).name;
        // Compare against the id as well as empty: displayNameFor() returns the
        // user id, never "", for unknown members, and the seed's caller
        // pre-fills this key with that call, so an isEmpty() check would leave
        // the row unresolvable for the session.
        if (e.roomName.isEmpty() || e.roomName == e.roomId) {
            e.roomName = e.roomId;
            e.roomNamePending = true;   // an id is a placeholder, not a name
        }
        e.senderName = m.value(QStringLiteral("senderName")).toString();
        if (e.senderName.isEmpty() || e.senderName == e.senderId) {
            e.senderName = e.senderId;
            e.senderNamePending = true;
        }
        e.preview = collapse(m.value(QStringLiteral("preview")).toString());
        e.encrypted = m.value(QStringLiteral("encrypted")).toBool();
        e.timestampMs = m.value(QStringLiteral("timestampMs")).toLongLong();
        e.eventId = e.id;
        e.threadRootId = m.value(QStringLiteral("threadRootId")).toString();
        // The server's read flag counts as seen, as in Element, but it is not
        // trusted alone; see reconcileSeedAgainstRoomCounts().
        e.seenMark = m.value(QStringLiteral("read")).toBool();
        m_ids.insert(e.id);
        seeded.append(e.id);
        m_entries.append(std::move(e));
        any = true;
    }
    if (!any)
        return;
    std::stable_sort(m_entries.begin(), m_entries.end(),
                     [](const Entry &a, const Entry &b) { return a.timestampMs > b.timestampMs; });
    while (m_entries.size() > kMaxEntries) {
        m_ids.remove(m_entries.last().id);
        m_entries.removeLast();
    }
    m_seedAwaitingRooms += reconcileSeedAgainstRoomCounts(seeded);
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

// The bell and the room list must not disagree about the same account.
//
// The seed is `GET /notifications?only=highlight`, each with its own `read`
// flag, which can stay false long after the room was read. The room list uses
// `highlight_count` (the bridge reports max(num_unread_mentions, sync
// highlight_count)); when they disagree the room list wins.
//
// Per room, the server says N highlights are unread: the newest N seeded rows
// stay unseen and the rest are read. Exact when the seed holds the room's
// whole backlog, conservative otherwise.
//
// Only rooms the client knows take part: roomInfo() returns a default
// RoomInfo for an unknown room, whose zero count means "unknown", not
// "nothing unread". Name and count arrive in the same payload, so a room with
// a resolved name has an equally fresh count.
//
// A row the server already called read is never un-marked.
QStringList ActivityModel::reconcileSeedAgainstRoomCounts(const QStringList &seededIds,
                                                          bool *flippedAny)
{
    if (flippedAny)
        *flippedAny = false;
    if (!m_client || seededIds.isEmpty())
        return {};
    const QSet<QString> seeded(seededIds.begin(), seededIds.end());

    // Rooms in this batch, each with its unread-highlight budget.
    QHash<QString, int> budget;
    QSet<QString> unknownRooms;
    for (const Entry &e : m_entries) {
        if (!seeded.contains(e.id) || budget.contains(e.roomId))
            continue;
        const RoomInfo info = m_client->roomInfo(e.roomId);
        if (info.id != e.roomId) {
            // Unknown room: leave its rows to the server's flag for now and
            // return them to the caller. A room is reconciled all-or-nothing,
            // so a retry cannot double-spend its budget.
            unknownRooms.insert(e.roomId);
            continue;
        }
        budget.insert(e.roomId, std::max(0, info.highlightCount));
    }

    QStringList deferred;
    if (!unknownRooms.isEmpty()) {
        for (const Entry &e : m_entries) {
            if (seeded.contains(e.id) && unknownRooms.contains(e.roomId))
                deferred.append(e.id);
        }
    }
    if (budget.isEmpty())
        return deferred;

    // m_entries is newest-first, so each room's budget goes to its newest rows.
    for (Entry &e : m_entries) {
        if (e.seenMark || !seeded.contains(e.id))
            continue;
        auto it = budget.find(e.roomId);
        if (it == budget.end())
            continue;
        if (*it > 0) {
            --*it;      // this one really is unread
            continue;
        }
        e.seenMark = true;
        if (flippedAny)
            *flippedAny = true;
    }
    return deferred;
}

// Replace the seed's placeholder names (raw room and user ids) once real
// names arrive with the first room payload or /members fetch. Rows only ever
// move away from a placeholder, so a later empty answer cannot un-name one.
void ActivityModel::resolvePendingSeedNames(const QString &roomId)
{
    if (!m_client)
        return;
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (!e.roomNamePending && !e.senderNamePending)
            continue;
        if (!roomId.isEmpty() && e.roomId != roomId)
            continue;
        bool changed = false;
        if (e.roomNamePending) {
            const RoomInfo info = m_client->roomInfo(e.roomId);
            if (info.id == e.roomId && !info.name.isEmpty()) {
                e.roomName = info.name;
                e.roomNamePending = false;
                changed = true;
            }
        }
        if (e.senderNamePending) {
            const QString name = m_client->displayNameFor(e.roomId, e.senderId);
            if (!name.isEmpty() && name != e.senderId) {
                e.senderName = name;
                e.senderNamePending = false;
                changed = true;
            }
        }
        if (!changed)
            continue;
        // Only two strings changed, so dataChanged on the visible index, never
        // a reset.
        const int visibleRow = m_visible.indexOf(i);
        if (visibleRow >= 0) {
            const QModelIndex idx = index(visibleRow, 0);
            Q_EMIT dataChanged(idx, idx,
                               { RoomNameRole, SenderNameRole });
        }
    }
}

bool ActivityModel::addEntry(Entry entry)
{
    if (m_ids.contains(entry.id))
        return false;
    m_ids.insert(entry.id);
    // Newest first; a live append is usually newest, but a backlog burst can
    // arrive out of order.
    int pos = 0;
    while (pos < m_entries.size() && m_entries.at(pos).timestampMs > entry.timestampMs)
        ++pos;
    const bool visible = passesFilter(entry);
    m_entries.insert(pos, std::move(entry));
    if (visible) {
        // Every visible index at or after pos shifts by one.
        int row = 0;
        while (row < m_visible.size() && m_visible.at(row) < pos)
            ++row;
        beginInsertRows(QModelIndex(), row, row);
        for (int i = 0; i < m_visible.size(); ++i)
            if (m_visible.at(i) >= pos)
                m_visible[i] += 1;
        m_visible.insert(row, pos);
        endInsertRows();
        Q_EMIT countChanged();
    } else {
        for (int i = 0; i < m_visible.size(); ++i)
            if (m_visible.at(i) >= pos)
                m_visible[i] += 1;
    }
    if (m_entries.size() > kMaxEntries) {
        const int last = m_entries.size() - 1;
        m_ids.remove(m_entries.last().id);
        const int row = m_visible.indexOf(last);
        if (row >= 0) {
            beginRemoveRows(QModelIndex(), row, row);
            m_visible.removeAt(row);
            endRemoveRows();
            Q_EMIT countChanged();
        }
        m_entries.removeLast();
    }
    Q_EMIT unseenCountChanged();
    return true;
}
