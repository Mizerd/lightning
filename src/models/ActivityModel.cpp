#include "models/ActivityModel.h"

#include "matrix/MatrixClient.h"

#include <QDateTime>
#include <QRegularExpression>

#include <algorithm>

namespace {
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
    // Nothing later than "now" can be marked from here: an entry with a
    // future (skewed) timestamp stays unseen rather than pushing the
    // marker past real time.
    m_seenUpToMs = std::min(newest, QDateTime::currentMSecsSinceEpoch());
    saveStore();
    if (!m_visible.isEmpty())
        Q_EMIT dataChanged(index(0), index(m_visible.size() - 1), { SeenRole });
    Q_EMIT unseenCountChanged();
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
    // Whole-word, case-insensitive; a keyword that itself starts or ends
    // with punctuation (e.g. "#lightning") still needs a boundary on the
    // side that has a word character.
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
    // A thread the user started (their own message became a root) or took
    // part in (their own reply names the root) is "their" thread.
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
    e.senderId = event.sender;
    e.senderName = event.senderDisplayName.isEmpty() ? event.sender
                                                     : event.senderDisplayName;
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
    e.senderId = senderId;
    e.senderName = senderName.isEmpty() ? senderId : senderName;
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
    e.senderId = room.inviterUserId;
    e.senderName = room.inviterDisplayName.isEmpty() ? room.inviterUserId
                                                     : room.inviterDisplayName;
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
    for (const QVariant &v : entries) {
        const QVariantMap m = v.toMap();
        Entry e;
        e.id = m.value(QStringLiteral("eventId")).toString();
        e.roomId = m.value(QStringLiteral("roomId")).toString();
        e.senderId = m.value(QStringLiteral("senderId")).toString();
        if (e.id.isEmpty() || e.roomId.isEmpty() || m_ids.contains(e.id)
            || e.senderId == self)
            continue;
        e.kind = m.value(QStringLiteral("kind")).toString();
        if (e.kind.isEmpty())
            e.kind = QStringLiteral("mention");
        e.roomName = m.value(QStringLiteral("roomName")).toString();
        if (e.roomName.isEmpty() && m_client)
            e.roomName = m_client->roomInfo(e.roomId).name;
        if (e.roomName.isEmpty())
            e.roomName = e.roomId;
        e.senderName = m.value(QStringLiteral("senderName")).toString();
        if (e.senderName.isEmpty())
            e.senderName = e.senderId;
        e.preview = collapse(m.value(QStringLiteral("preview")).toString());
        e.encrypted = m.value(QStringLiteral("encrypted")).toBool();
        e.timestampMs = m.value(QStringLiteral("timestampMs")).toLongLong();
        e.eventId = e.id;
        e.threadRootId = m.value(QStringLiteral("threadRootId")).toString();
        // The server's own read flag counts as seen: it is what Element
        // shows, and re-surfacing a mention the user already dealt with
        // elsewhere would be noise.
        e.seenMark = m.value(QStringLiteral("read")).toBool();
        m_ids.insert(e.id);
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
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

bool ActivityModel::addEntry(Entry entry)
{
    if (m_ids.contains(entry.id))
        return false;
    m_ids.insert(entry.id);
    // Newest first; a live append is almost always the newest, but a
    // backlog burst can arrive out of order.
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
