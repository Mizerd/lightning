#include "models/ActivityModel.h"

#include "matrix/EventPreview.h"
#include "matrix/MatrixClient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lcActivity, "lightning.activity")

namespace {
QString redactedRoom(const QString &id)
{
    if (id.isEmpty())
        return QStringLiteral("<none>");
    const bool sigil = id.front() == QLatin1Char('!');
    const QByteArray digest = QCryptographicHash::hash(
        (sigil ? id.mid(1) : id).toUtf8(), QCryptographicHash::Sha256);
    return (sigil ? QStringLiteral("!") : QString())
        + QString::fromLatin1(digest.toHex().left(8));
}

const QStringList kKnownKinds{
    QStringLiteral("mention"),      QStringLiteral("room_mention"),
    QStringLiteral("reply"),        QStringLiteral("thread"),
    QStringLiteral("reaction"),     QStringLiteral("invite"),
    QStringLiteral("keyword"),      QStringLiteral("highlight"),
    QStringLiteral("dm"),
};
constexpr int kMaxOwnEventIds = 2048;
constexpr int kMaxRedactedIds = 512;

bool isHighlightKind(const QString &kind)
{
    return kind == QLatin1String("mention") || kind == QLatin1String("room_mention")
        || kind == QLatin1String("keyword") || kind == QLatin1String("highlight");
}

QString collapse(const QString &text)
{
    QString out = text.simplified();
    if (out.size() > ActivityModel::kPreviewChars)
        out = out.left(ActivityModel::kPreviewChars - 1) + QChar(0x2026);
    return out;
}
}

ActivityModel::ActivityModel(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(this, &QAbstractItemModel::dataChanged, this, [this] {
        ++m_threadActivityRevision;
        Q_EMIT threadActivityChanged();
    });
}

void ActivityModel::setStore(Store store)
{
    m_store = std::move(store);
    m_storeLoaded = false;
}

void ActivityModel::setIgnoredCheck(std::function<bool(const QString &)> isIgnored)
{
    m_isIgnored = std::move(isIgnored);
}

void ActivityModel::setClock(std::function<qint64()> now)
{
    m_now = std::move(now);
}

qint64 ActivityModel::now() const
{
    return m_now ? m_now() : QDateTime::currentMSecsSinceEpoch();
}

bool ActivityModel::isIgnored(const QString &userId) const
{
    return m_isIgnored && !userId.isEmpty() && m_isIgnored(userId);
}

QString ActivityModel::realRoomId(const QString &roomId)
{
    return MatrixClient::isThreadTimelineId(roomId)
        ? MatrixClient::threadTimelineRoomId(roomId)
        : roomId;
}

// A thread timeline's copy carries the composite timeline id (§8: it must
// never reach a role, a read path or the mute lookup). Reduce it to the room,
// and take the root from it when the event did not say.
TimelineEvent ActivityModel::normalised(const TimelineEvent &event)
{
    if (!MatrixClient::isThreadTimelineId(event.roomId))
        return event;
    TimelineEvent out = event;
    out.roomId = MatrixClient::threadTimelineRoomId(event.roomId);
    if (out.threadRootId.isEmpty())
        out.threadRootId = MatrixClient::threadTimelineRootId(event.roomId);
    return out;
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
    connect(m_client, &MatrixClient::threadSubscriptionState, this,
            [this](const QString &roomId, const QString &rootId,
                   bool supported, bool subscribed, bool) {
        const QString key = roomId + QChar(0x1f) + rootId;
        const int state = !supported ? 1 : (subscribed ? 3 : 2);
        if (m_threadFollowing.value(key) == state)
            return;
        m_threadFollowing.insert(key, state);
        ++m_threadActivityRevision;
        Q_EMIT threadActivityChanged();
    });
    connect(m_client, &MatrixClient::activitySeedCursorReceived, this,
            [this](const QString &next) {
        m_threadPageToken = next;
        m_threadPageLoading = false;
        m_threadPageFailed = false;
        Q_EMIT threadPageChanged();
    });
    connect(m_client, &MatrixClient::activitySeedPageReceived, this,
            [this](const QVariantList &entries, const QString &next) {
        m_threadPageToken = next;
        m_threadPageLoading = false;
        m_threadPageFailed = false;
        seed(entries);
        refreshThreadFollowing();
        Q_EMIT threadPageChanged();
    });
    connect(m_client, &MatrixClient::activitySeedPageFailed, this,
            [this](const QString &) {
        m_threadPageLoading = false;
        m_threadPageFailed = true;
        Q_EMIT threadPageChanged();
    });
    connect(m_client, &MatrixClient::loginSucceeded, this,
            [this](const QString &) { scheduleStoreLoad(); });
    connect(m_client, &MatrixClient::connectionStateChanged, this,
            [this](MatrixClient::ConnectionState state) {
        if (state == MatrixClient::Syncing)
            loadStore();
    });
    if (m_client->isLoggedIn() && !m_client->currentUserId().isEmpty())
        scheduleStoreLoad();
    connect(m_client, &MatrixClient::eventRedacted, this,
            &ActivityModel::eventRedacted);
    connect(m_client, &MatrixClient::eventDecrypted, this,
            &ActivityModel::eventDecrypted);
    connect(m_client, &MatrixClient::inviteAnswered, this,
            [this](const QString &roomId, bool) { inviteAnswered(roomId); });
    connect(m_client, &MatrixClient::activitySeedFailed, this,
            [this](const QString &category) {
        if (seedFailureIsPermanent(category))
            setHistoryUnavailable(true);
    });
    connect(m_client, &MatrixClient::activitySeedReceived, this,
            [this] { setHistoryUnavailable(false); });
    connect(m_client, &MatrixClient::roomsChanged, this,
            &ActivityModel::pruneRoomsNoLongerJoined);
    connect(m_client, &MatrixClient::roomsChanged, this,
            &ActivityModel::syncDirectUnreads);
    connect(m_client, &MatrixClient::roomsChanged, this,
            &ActivityModel::reconcileRoomsAgainstTheirReadState);
    connect(m_client, &MatrixClient::roomsChanged, this, [this] {
        resolvePendingSeedNames();
        if (m_seedAwaitingRooms.isEmpty())
            return;
        const QStringList pending = std::move(m_seedAwaitingRooms);
        bool flipped = false;
        m_seedAwaitingRooms =
            reconcileSeedAgainstRoomCounts(pending, &flipped);
        if (flipped) {
            if (!m_visible.isEmpty())
                Q_EMIT dataChanged(index(0), index(m_visible.size() - 1),
                                   { SeenRole });
            Q_EMIT unseenCountChanged();
        }
    });
    connect(m_client, &MatrixClient::membersChanged, this,
            [this](const QString &roomId) { resolvePendingSeedNames(roomId); });
    connect(m_client, &MatrixClient::roomsChanged, this,
            &ActivityModel::refreshFavourites);
    connect(m_client, &MatrixClient::roomUpdated, this,
            [this](const QString &) { refreshFavourites(); });
}

void ActivityModel::scheduleStoreLoad()
{
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(this, [this, generation] {
        // A sign-out or switch in between cleared the model; that account's
        // load must not land on the next one.
        if (generation == m_generation)
            loadStore();
    }, Qt::QueuedConnection);
}

void ActivityModel::loadStore()
{
    if (m_storeLoaded || !m_store.load)
        return;
    if (!m_client || m_client->currentUserId().isEmpty())
        return;
    m_storeLoaded = true;
    const QVariantMap saved = m_store.load();
    const qint64 stored = saved.value(QStringLiteral("seenUpToMs")).toLongLong();
    m_seenLoadedUpToMs = std::max<qint64>(0, stored);
    const bool markerMoved = m_seenUpToMs > m_seenLoadedUpToMs;
    m_seenUpToMs = std::max(m_seenUpToMs, m_seenLoadedUpToMs);
    m_invites.clear();
    const QVariantMap invites = saved.value(QStringLiteral("invites")).toMap();
    for (auto it = invites.cbegin(); it != invites.cend(); ++it) {
        const QVariantMap rec = it.value().toMap();
        const qint64 t = rec.value(QStringLiteral("t")).toLongLong();
        if (it.key().isEmpty() || t <= 0)
            continue;
        m_invites.insert(it.key(), QVariantMap{
            { QStringLiteral("t"), t },
            { QStringLiteral("seen"), rec.value(QStringLiteral("seen")).toBool() } });
    }
    bool invitesMoved = false;
    for (Entry &e : m_entries) {
        if (e.kind != QLatin1String("invite"))
            continue;
        const QVariantMap rec = m_invites.value(e.roomId).toMap();
        if (rec.isEmpty()) {
            m_invites.insert(e.roomId, QVariantMap{
                { QStringLiteral("t"), e.timestampMs },
                { QStringLiteral("seen"), e.seenMark } });
            invitesMoved = true;
            continue;
        }
        e.timestampMs = rec.value(QStringLiteral("t")).toLongLong();
        e.seenMark = e.seenMark || rec.value(QStringLiteral("seen")).toBool();
    }
    std::stable_sort(m_entries.begin(), m_entries.end(),
                     [](const Entry &a, const Entry &b) { return a.timestampMs > b.timestampMs; });
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
    if (markerMoved || invitesMoved)
        saveStore();
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

void ActivityModel::saveStore()
{
    // Before the load, the store's key may still name another account, and
    // what this model holds is not that account's state: writing it would
    // replace the real marker, keywords and invites.
    if (!m_store.save || !m_storeLoaded)
        return;
    m_store.save(QVariantMap{
        { QStringLiteral("seenUpToMs"), m_seenUpToMs },
        { QStringLiteral("keywords"), m_keywords },
        { QStringLiteral("invites"), m_invites },
    });
}

void ActivityModel::rememberInvite(const QString &roomId, qint64 firstNotedMs,
                                   bool seen)
{
    if (roomId.isEmpty())
        return;
    m_invites.insert(roomId, QVariantMap{ { QStringLiteral("t"), firstNotedMs },
                                          { QStringLiteral("seen"), seen } });
    while (m_invites.size() > kMaxRememberedInvites) {
        auto oldest = m_invites.begin();
        for (auto it = m_invites.begin(); it != m_invites.end(); ++it) {
            if (it.value().toMap().value(QStringLiteral("t")).toLongLong()
                < oldest.value().toMap().value(QStringLiteral("t")).toLongLong())
                oldest = it;
        }
        m_invites.erase(oldest);
    }
    saveStore();
}

void ActivityModel::forgetInvite(const QString &roomId)
{
    if (m_invites.remove(roomId) > 0)
        saveStore();
}

bool ActivityModel::persistSeenInvites()
{
    bool moved = false;
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.kind != QLatin1String("invite") || !isSeen(e))
            continue;
        auto it = m_invites.find(e.roomId);
        if (it == m_invites.end()) {
            m_invites.insert(e.roomId, QVariantMap{
                { QStringLiteral("t"), e.timestampMs },
                { QStringLiteral("seen"), true } });
            moved = true;
            continue;
        }
        QVariantMap rec = it.value().toMap();
        if (rec.value(QStringLiteral("seen")).toBool())
            continue;
        rec.insert(QStringLiteral("seen"), true);
        it.value() = rec;
        moved = true;
    }
    return moved;
}

void ActivityModel::clear()
{
    beginResetModel();
    m_searchQuery.clear();
    Q_EMIT searchQueryChanged();
    if (m_favouritesOnly) {
        m_favouritesOnly = false;
        Q_EMIT favouritesOnlyChanged();
    }
    m_favouriteRoomIds.clear();
    m_entries.clear();
    m_visible.clear();
    m_ids.clear();
    m_ownEventIds.clear();
    m_ownEventOrder.clear();
    m_ownThreadRoots.clear();
    m_seedAwaitingRooms.clear();
    m_seenUpToMs = 0;
    m_seenLoadedUpToMs = 0;
    m_storeLoaded = false;
    ++m_generation;
    m_invites.clear();
    m_roomMissingSince.clear();
    m_roomsEverKnown.clear();
    m_redactedIds.clear();
    m_redactedOrder.clear();
    m_answeredInvites.clear();
    // Room notification modes are account-scoped: a room two accounts share
    // can be muted for one and not the other.
    m_muteCache.clear();
    m_threadFollowing.clear();
    m_threadPageToken.clear();
    m_threadPageLoading = false;
    m_threadPageFailed = false;
    const bool keywordsMoved = !m_keywords.isEmpty();
    m_keywords.clear();
    endResetModel();
    if (keywordsMoved)
        Q_EMIT keywordsChanged();
    setHistoryUnavailable(false);
    Q_EMIT countChanged();
    Q_EMIT unseenCountChanged();
    ++m_threadActivityRevision;
    Q_EMIT threadActivityChanged();
    Q_EMIT threadPageChanged();
}

void ActivityModel::setHistoryUnavailable(bool unavailable)
{
    if (m_historyUnavailable == unavailable)
        return;
    m_historyUnavailable = unavailable;
    Q_EMIT historyUnavailableChanged();
}

void ActivityModel::setReadHorizonDays(int days)
{
    days = std::clamp(days, 0, 3650);
    if (days == m_readHorizonDays)
        return;
    m_readHorizonDays = days;
    if (days > 0) {
        const qint64 cutoff = now() - qint64(days) * 86400000LL;
        removeEntries([cutoff](const Entry &e) {
            return e.readMark && e.kind != QLatin1String("invite")
                && e.timestampMs > 0 && e.timestampMs < cutoff;
        });
    }
    Q_EMIT readHorizonDaysChanged();
}

bool ActivityModel::seedFailureIsPermanent(const QString &category)
{
    if (category.endsWith(QLatin1String("_M_UNRECOGNIZED")))
        return true;
    return category.startsWith(QLatin1String("http_404_"))
        || category.startsWith(QLatin1String("http_405_"))
        || category.startsWith(QLatin1String("http_501_"));
}

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

bool ActivityModel::isSeen(const Entry &e) const
{
    return e.seenMark
        || (e.timestampMs > 0 && e.timestampMs <= m_seenLoadedUpToMs);
}

bool ActivityModel::isMutedRoom(const Entry &e) const
{
    if (!m_modeOf || e.kind == QLatin1String("invite") || e.roomId.isEmpty())
        return false;
    const auto cached = m_muteCache.constFind(e.roomId);
    if (cached != m_muteCache.constEnd())
        return *cached;
    const bool muted = m_modeOf(e.roomId) == 2;
    m_muteCache.insert(e.roomId, muted);
    return muted;
}

void ActivityModel::setNotificationModeSource(
    std::function<int(const QString &)> modeOf)
{
    m_modeOf = std::move(modeOf);
    notificationModesChanged();
}

void ActivityModel::notificationModesChanged()
{
    m_muteCache.clear();
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

void ActivityModel::setReactionsVisible(bool visible)
{
    if (m_reactionsVisible == visible)
        return;
    m_reactionsVisible = visible;
    if (!visible && m_filter == QLatin1String("reactions"))
        setFilter(QStringLiteral("all"));
    rebuildVisible();
    Q_EMIT reactionsVisibleChanged();
    Q_EMIT unseenCountChanged();
}

int ActivityModel::unseenCount() const
{
    int n = 0;
    for (const Entry &e : m_entries)
        if (!isSeen(e) && !isMutedRoom(e)
            && (m_reactionsVisible || e.kind != QLatin1String("reaction")))
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
    m_seenUpToMs = std::min(newest, QDateTime::currentMSecsSinceEpoch());
    persistSeenInvites();
    saveStore();
    if (!m_visible.isEmpty())
        Q_EMIT dataChanged(index(0), index(m_visible.size() - 1), { SeenRole });
    Q_EMIT unseenCountChanged();
}

void ActivityModel::markRoomReadNow(const QString &roomId)
{
    markRoomReadUpTo(roomId, now());
}

void ActivityModel::markRoomReadUpTo(const QString &rawRoomId, qint64 timestampMs,
                                     const QString &threadRootId)
{
    const QString roomId = realRoomId(rawRoomId);
    if (roomId.isEmpty() || timestampMs <= 0)
        return;
    const qint64 readAtMs = now();
    bool changed = false;
    for (Entry &e : m_entries) {
        if (e.roomId != roomId)
            continue;
        if (e.kind == QLatin1String("invite"))
            continue;
        if (!threadRootId.isEmpty() && e.threadRootId != threadRootId)
            continue;
        if (e.kind == QLatin1String("reaction") && threadRootId.isEmpty()
            && e.arrivedMs > 0 && e.arrivedMs < readAtMs) {
            changed = !e.readMark || !e.seenMark || changed;
            e.readMark = true;
            e.seenMark = true;
            continue;
        }
        if (e.timestampMs <= 0 || e.timestampMs > timestampMs)
            continue;
        changed = !e.readMark || !e.seenMark || changed;
        e.readMark = true;
        e.seenMark = true;
    }
    if (!changed)
        return;
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

void ActivityModel::syncDirectUnreads()
{
    if (!m_client || !m_client->tracksRoomReadState())
        return;
    const QList<RoomInfo> rooms = m_client->rooms();
    for (const RoomInfo &info : rooms) {
        if (!info.isDirect || info.membership != RoomInfo::Joined
            || info.id.isEmpty() || info.directUserId.isEmpty())
            continue;
        const QString id = QStringLiteral("dm:") + info.id;
        const QString preview =
            collapse(matrix::preview::stripReplyFallback(info.lastMessagePreview));
        qint64 listedAt = -1;
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &e = m_entries[i];
            if (e.id != id)
                continue;
            listedAt = e.timestampMs;
            if (e.preview != preview) {
                e.preview = preview;
                const int row = m_visible.indexOf(i);
                if (row >= 0)
                    Q_EMIT dataChanged(index(row), index(row), { PreviewRole });
            }
            break;
        }
        const bool unread = info.unreadCount > info.highlightCount
            && !isIgnored(info.directUserId);
        if (!unread)
            continue;
        const qint64 ts = info.lastActivity.isValid()
            ? info.lastActivity.toMSecsSinceEpoch()
            : (listedAt >= 0 ? listedAt : now());
        if (listedAt >= 0 && ts <= listedAt)
            continue;
        removeEntries([&id](const Entry &e) { return e.id == id; });
        Entry e;
        e.id = id;
        e.kind = QStringLiteral("dm");
        e.roomId = info.id;
        e.roomName = info.name;
        e.senderId = info.directUserId;
        e.senderName = m_client->displayNameFor(info.id, info.directUserId);
        e.preview = preview;
        e.timestampMs = ts;
        if (e.roomName.isEmpty() || e.roomName == e.roomId) {
            e.roomName = e.senderName;
            if (e.roomName.isEmpty())
                e.roomName = e.senderId;
        }
        addEntry(std::move(e));
    }
}

void ActivityModel::reconcileRoomsAgainstTheirReadState()
{
    if (!m_client || !m_client->tracksRoomReadState())
        return;
    QSet<QString> rooms;
    for (const Entry &e : m_entries) {
        if (!e.readMark && e.kind != QLatin1String("invite"))
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
    bool holds = false;
    for (const Entry &e : m_entries) {
        if (!e.readMark && e.roomId == roomId
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
    // The SDK's client-side counts skip thread events and an encrypted room
    // has no server count to fall back on, so a clear encrypted room says
    // nothing about its threads. Fail closed while encryption is unknown.
    const bool countsCoverThreads = !info.encrypted && info.encryptionKnown;
    if (info.markedUnread || info.hasUnreadMessages || info.unreadCount > 0
        || info.highlightCount > 0) {
        return false;
    }
    if (!info.lastActivity.isValid())
        return false;
    const qint64 readUpToMs = info.lastActivity.toMSecsSinceEpoch();
    if (readUpToMs <= 0)
        return false;
    int marked = 0;
    qint64 nearestLagMs = -1;
    for (Entry &e : m_entries) {
        if (e.roomId != roomId)
            continue;
        if (e.kind == QLatin1String("invite"))
            continue;
        if (!e.threadRootId.isEmpty() && !countsCoverThreads)
            continue;
        // Only a counter write after the row arrived can speak for it:
        // roomsChanged fires for any room, and a payload from before the row
        // reads as "read" the instant a new mention lands.
        if (info.countsWrittenAtMs <= e.arrivedMs)
            continue;
        if (e.timestampMs <= 0 || e.timestampMs > readUpToMs)
            continue;
        if (e.readMark)
            continue;
        e.readMark = true;
        e.seenMark = true;
        ++marked;
        const qint64 lagMs = info.countsWrittenAtMs - e.arrivedMs;
        if (nearestLagMs < 0 || lagMs < nearestLagMs)
            nearestLagMs = lagMs;
    }
    if (marked > 0) {
        qCInfo(lcActivity) << "rows marked seen by room read state room="
                           << redactedRoom(roomId) << "rows=" << marked
                           << "counts_after_row_ms=" << nearestLagMs
                           << "encrypted=" << info.encrypted;
    }
    return marked > 0;
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
        if (e.kind == QLatin1String("invite") && persistSeenInvites())
            saveStore();
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

QVariantList ActivityModel::threadActivity(const QString &scope) const
{
    QVariantList rows;
    QHash<QString, int> positions;
    QHash<QString, QHash<QString, QString>> loadedRoots;
    for (const Entry &e : m_entries) {
        if (e.threadRootId.isEmpty() || e.eventId.isEmpty()
            || isMutedRoom(e) || e.kind == QLatin1String("reaction")
            || !passesFavourites(e))
            continue;
        const QString key = e.roomId + QChar(0x1f) + e.threadRootId;
        int pos = positions.value(key, -1);
        if (pos < 0) {
            if (m_client && !loadedRoots.contains(e.roomId)) {
                QHash<QString, QString> previews;
                for (const TimelineEvent &item : m_client->timeline(e.roomId))
                    if (!item.eventId.isEmpty())
                        previews.insert(item.eventId, previewOf(item));
                loadedRoots.insert(e.roomId, previews);
            }
            pos = rows.size();
            positions.insert(key, pos);
            rows.append(QVariantMap{
                { QStringLiteral("roomId"), e.roomId },
                { QStringLiteral("roomName"), e.roomName },
                { QStringLiteral("rootEventId"), e.threadRootId },
                { QStringLiteral("eventId"), e.eventId },
                { QStringLiteral("senderName"), e.senderName },
                { QStringLiteral("preview"), e.preview },
                { QStringLiteral("rootPreview"),
                  loadedRoots.value(e.roomId).value(e.threadRootId) },
                { QStringLiteral("encrypted"), e.encrypted },
                { QStringLiteral("timestampMs"), e.timestampMs },
                { QStringLiteral("unread"), false },
                { QStringLiteral("mention"), false },
                { QStringLiteral("relevant"), false },
                { QStringLiteral("participating"), false },
                { QStringLiteral("following"), m_threadFollowing.value(key, 0) == 3 },
                { QStringLiteral("followSupported"), m_threadFollowing.value(key, 0) >= 2 },
            });
        }
        QVariantMap row = rows.at(pos).toMap();
        row.insert(QStringLiteral("unread"),
                   row.value(QStringLiteral("unread")).toBool() || !e.readMark);
        const bool mention = e.kind == QLatin1String("mention")
            || e.kind == QLatin1String("room_mention")
            || e.kind == QLatin1String("highlight");
        row.insert(QStringLiteral("mention"),
                   row.value(QStringLiteral("mention")).toBool()
                       || (mention && !e.readMark));
        row.insert(QStringLiteral("relevant"), true);
        row.insert(QStringLiteral("participating"),
                   row.value(QStringLiteral("participating")).toBool()
                       || e.kind == QLatin1String("thread")
                       || e.kind == QLatin1String("reply"));
        rows[pos] = row;
    }
    QVariantList filtered;
    for (const QVariant &v : rows) {
        const QVariantMap row = v.toMap();
        if (scope == QLatin1String("unread")
            && !row.value(QStringLiteral("unread")).toBool())
            continue;
        if (scope == QLatin1String("following")
            && !row.value(QStringLiteral("following")).toBool())
            continue;
        if (scope == QLatin1String("participating")
            && !row.value(QStringLiteral("participating")).toBool())
            continue;
        const QString query = m_searchQuery.trimmed();
        if (!query.isEmpty()
            && !row.value(QStringLiteral("roomName")).toString().contains(query, Qt::CaseInsensitive)
            && !row.value(QStringLiteral("senderName")).toString().contains(query, Qt::CaseInsensitive)
            && !(row.value(QStringLiteral("encrypted")).toBool()
                 ? tr("Encrypted message") : row.value(QStringLiteral("preview")).toString()).contains(query, Qt::CaseInsensitive)
            && !row.value(QStringLiteral("rootPreview")).toString().contains(query, Qt::CaseInsensitive))
            continue;
        filtered.append(row);
    }
    return filtered;
}

void ActivityModel::refreshThreadFollowing()
{
    if (!m_client)
        return;
    int asked = 0;
    for (const QVariant &v : threadActivity(QStringLiteral("all"))) {
        const QVariantMap row = v.toMap();
        const QString roomId = row.value(QStringLiteral("roomId")).toString();
        const QString rootId = row.value(QStringLiteral("rootEventId")).toString();
        const QString key = roomId + QChar(0x1f) + rootId;
        if (m_threadFollowing.contains(key))
            continue;
        m_threadFollowing.insert(key, 1);
        m_client->queryThreadSubscription(roomId, rootId);
        if (++asked >= 50)
            break;
    }
}

void ActivityModel::loadOlderActivity()
{
    if (!m_client || m_threadPageLoading || !activityHasMore())
        return;
    m_threadPageLoading = true;
    m_threadPageFailed = false;
    Q_EMIT threadPageChanged();
    m_client->requestActivitySeedPage(m_threadPageToken, 60);
}

void ActivityModel::openThreadActivity(const QString &roomId,
                                       const QString &rootEventId,
                                       const QString &eventId)
{
    if (roomId.isEmpty() || rootEventId.isEmpty() || eventId.isEmpty()
        || MatrixClient::isThreadTimelineId(roomId))
        return;
    markSeen(eventId);
    Q_EMIT openRequested(roomId, eventId, rootEventId);
}

void ActivityModel::setSearchQuery(const QString &query)
{
    if (m_searchQuery == query)
        return;
    m_searchQuery = query;
    rebuildVisible();
    Q_EMIT searchQueryChanged();
}

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

QSet<QString> ActivityModel::favouriteRoomIds() const
{
    QSet<QString> ids;
    if (!m_client)
        return ids;
    for (const RoomInfo &room : m_client->rooms())
        if (room.isFavourite)
            ids.insert(room.id);
    return ids;
}

void ActivityModel::refreshFavourites()
{
    if (!m_favouritesOnly)
        return;
    QSet<QString> ids = favouriteRoomIds();
    if (ids == m_favouriteRoomIds)
        return;
    m_favouriteRoomIds = std::move(ids);
    rebuildVisible();
}

bool ActivityModel::passesFavourites(const Entry &e) const
{
    return !m_favouritesOnly || m_favouriteRoomIds.contains(realRoomId(e.roomId));
}

void ActivityModel::setFavouritesOnly(bool on)
{
    if (m_favouritesOnly == on)
        return;
    m_favouritesOnly = on;
    m_favouriteRoomIds = on ? favouriteRoomIds() : QSet<QString>();
    rebuildVisible();
    Q_EMIT favouritesOnlyChanged();
}

bool ActivityModel::passesFilter(const Entry &e) const
{
    if (!passesFavourites(e))
        return false;
    const QString query = m_searchQuery.trimmed();
    if (!query.isEmpty()
        && !e.roomName.contains(query, Qt::CaseInsensitive)
        && !e.senderName.contains(query, Qt::CaseInsensitive)
        && !e.senderId.contains(query, Qt::CaseInsensitive)
        && !(e.encrypted ? tr("Encrypted message") : e.preview).contains(query, Qt::CaseInsensitive)
        && !e.reactionKey.contains(query, Qt::CaseInsensitive))
        return false;
    if (isMutedRoom(e)
        || (!m_reactionsVisible && e.kind == QLatin1String("reaction")))
        return false;
    if (m_filter == QLatin1String("all"))
        return true;
    if (m_filter == QLatin1String("mentions"))
        return e.kind == QLatin1String("mention") || e.kind == QLatin1String("room_mention")
            || e.kind == QLatin1String("highlight");
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
    ++m_threadActivityRevision;
    Q_EMIT threadActivityChanged();
}

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
    if (event.isEdit)
        return event.mentionsMe
                && (event.pushVerdict == TimelineEvent::PushVerdict::Unknown
                    || event.pushHighlight)
            ? QStringLiteral("mention") : QString();
    if ((!event.replyToSenderId.isEmpty() && event.replyToSenderId == selfUserId)
        || (!event.replyToEventId.isEmpty() && ownEventIds.contains(event.replyToEventId)))
        return QStringLiteral("reply");
    if (event.mentionsMe
        && (event.pushVerdict == TimelineEvent::PushVerdict::Unknown
            || event.pushHighlight))
        return QStringLiteral("mention");
    if (!event.threadRootId.isEmpty()
        && (ownThreadRoots.contains(event.threadRootId)
            || ownEventIds.contains(event.threadRootId)))
        return QStringLiteral("thread");
    if (event.pushVerdict != TimelineEvent::PushVerdict::Unknown) {
        if (!event.pushHighlight)
            return {};
        return event.roomMentionHighlighted ? QStringLiteral("room_mention")
                                  : QStringLiteral("highlight");
    }
    if (!event.undecryptable) {
        for (const QString &k : keywords)
            if (matchesKeyword(matrix::preview::stripReplyFallback(event.body), k))
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
    return collapse(matrix::preview::stripReplyFallback(event.body));
}

QString ActivityModel::glyphForMsgtype(const QString &msgtype)
{
    if (msgtype == QLatin1String("m.image"))
        return QStringLiteral("📷");
    if (msgtype == QLatin1String("m.video"))
        return QStringLiteral("🎬");
    if (msgtype == QLatin1String("m.audio"))
        return QStringLiteral("🎵");
    if (msgtype == QLatin1String("m.file"))
        return QStringLiteral("📎");
    if (msgtype == QLatin1String("m.sticker"))
        return QStringLiteral("🩷");
    return {};
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
    if (event.isThreadRoot)
        m_ownThreadRoots.insert(event.eventId);
    if (!event.threadRootId.isEmpty())
        m_ownThreadRoots.insert(event.threadRootId);
}

bool ActivityModel::ingest(const TimelineEvent &rawEvent, const QString &roomName)
{
    if (!m_client)
        return false;
    const TimelineEvent event = normalised(rawEvent);
    const QString self = m_client->currentUserId();
    if (event.sender == self && !event.isLocalEcho && !event.isVirtual()) {
        if (!event.isEdit)
            rememberOwn(event);
        return false;
    }
    if (isIgnored(event.sender))
        return false;
    if (event.isEdit)
        return ingestEdit(event, roomName);
    if (settleProvisional(event.eventId, event.sender, event.roomId))
        return false;
    const QString kind = classify(event, self, m_ownEventIds, m_ownThreadRoots, m_keywords);
    if (kind.isEmpty() || event.eventId.isEmpty())
        return false;
    Entry e;
    e.id = event.eventId;
    e.kind = kind;
    e.highlight = isHighlightKind(kind) || event.pushHighlight;
    e.roomId = event.roomId;
    e.roomName = roomName;
    e.roomNamePending = roomName.isEmpty() || roomName == e.roomId;
    e.senderId = event.sender;
    e.senderName = event.senderDisplayName.isEmpty() ? event.sender
                                                     : event.senderDisplayName;
    e.senderNamePending = e.senderName == e.senderId;
    e.preview = previewOf(event);
    e.encrypted = event.undecryptable;
    e.timestampMs = event.timestamp.isValid() ? event.timestamp.toMSecsSinceEpoch() : 0;
    e.eventId = event.eventId;
    e.threadRootId = event.threadRootId;
    return addEntry(std::move(e));
}

bool ActivityModel::ingestEdit(const TimelineEvent &event, const QString &roomName)
{
    const bool highlightedMention = event.mentionsMe
        && (event.pushVerdict == TimelineEvent::PushVerdict::Unknown
            || event.pushHighlight);
    const QString target = event.replacesEventId;
    if (target.isEmpty() || m_redactedIds.contains(target))
        return false;
    const QString newPreview = event.undecryptable
        ? QString()
        : collapse(matrix::preview::stripReplyFallback(event.editNewBody));
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.id != target || e.kind == QLatin1String("reaction"))
            continue;
        // A replacement from anyone but the original's sender is invalid (spec,
        // m.replace), and the unopened-room path takes replaces_event_id as
        // sent: without this, any member could rewrite Alice's listed mention
        // and have the bell read "Alice: <their text>".
        if (e.senderId != event.sender || e.roomId != event.roomId)
            return false;
        QList<int> roles;
        if (!newPreview.isEmpty() && newPreview != e.preview) {
            e.preview = newPreview;
            e.encrypted = false;
            roles << PreviewRole << EncryptedRole;
        }
        if (highlightedMention && e.kind != QLatin1String("mention")
            && e.kind != QLatin1String("reply")) {
            e.kind = QStringLiteral("mention");
            e.highlight = true;
            roles << KindRole;
        }
        if (roles.isEmpty())
            return false;
        if (roles.contains(KindRole)) {
            rebuildVisible();
        } else {
            const int row = m_visible.indexOf(i);
            if (row >= 0)
                Q_EMIT dataChanged(index(row), index(row), roles);
        }
        return false;
    }
    if (!highlightedMention || isIgnored(event.sender))
        return false;
    Entry e;
    e.id = target;
    e.kind = QStringLiteral("mention");
    e.highlight = true;
    e.roomId = event.roomId;
    e.roomName = roomName;
    e.roomNamePending = roomName.isEmpty() || roomName == e.roomId;
    e.senderId = event.sender;
    e.senderName = event.senderDisplayName.isEmpty() ? event.sender
                                                     : event.senderDisplayName;
    e.senderNamePending = e.senderName == e.senderId;
    e.preview = newPreview;
    e.encrypted = event.undecryptable;
    e.timestampMs = event.timestamp.isValid() ? event.timestamp.toMSecsSinceEpoch() : 0;
    e.eventId = target;
    e.threadRootId = event.threadRootId;
    e.provisional = true;
    return addEntry(std::move(e));
}

bool ActivityModel::settleProvisional(const QString &eventId, const QString &sender,
                                      const QString &roomId)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.id != eventId || !e.provisional)
            continue;
        if (e.senderId == sender && e.roomId == roomId) {
            e.provisional = false;
            return true;
        }
        const QString forger = e.senderId;
        const QString forgedRoom = e.roomId;
        removeEntries([&](const Entry &x) {
            return x.provisional && x.id == eventId && x.senderId == forger
                && x.roomId == forgedRoom;
        });
        return false;
    }
    return false;
}

void ActivityModel::eventDecrypted(const QString &roomId, const TimelineEvent &decrypted)
{
    if (!m_client || decrypted.eventId.isEmpty())
        return;
    TimelineEvent event = decrypted;
    if (event.roomId.isEmpty())
        event.roomId = roomId;
    event = normalised(event);
    const QString self = m_client->currentUserId();
    if (event.sender == self) {
        if (!event.isEdit)
            rememberOwn(event);
        return;
    }
    if (isIgnored(event.sender) || m_redactedIds.contains(event.eventId))
        return;
    if (event.isEdit) {
        ingestEdit(event, m_client->roomInfo(event.roomId).name);
        return;
    }
    if (settleProvisional(event.eventId, event.sender, event.roomId))
        return;
    const QString kind =
        classify(event, self, m_ownEventIds, m_ownThreadRoots, m_keywords);
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.id != event.eventId || e.kind == QLatin1String("reaction"))
            continue;
        const bool kindMoved = !kind.isEmpty() && kind != e.kind;
        if (kindMoved)
            e.kind = kind;
        e.highlight = e.highlight || isHighlightKind(kind) || event.pushHighlight;
        e.preview = previewOf(event);
        e.encrypted = event.undecryptable;
        if (e.threadRootId.isEmpty())
            e.threadRootId = event.threadRootId;
        if (kindMoved) {
            rebuildVisible();
        } else {
            const int row = m_visible.indexOf(i);
            if (row >= 0)
                Q_EMIT dataChanged(index(row), index(row),
                                   { PreviewRole, EncryptedRole, ThreadRootIdRole });
        }
        Q_EMIT unseenCountChanged();
        return;
    }
    const RoomInfo info = m_client->roomInfo(event.roomId);
    ingest(event, info.name.isEmpty() ? event.roomId : info.name);
}

bool ActivityModel::removeEntries(const std::function<bool(const Entry &)> &drop)
{
    bool any = false;
    for (const Entry &e : std::as_const(m_entries)) {
        if (drop(e)) {
            m_ids.remove(e.id);
            m_seedAwaitingRooms.removeAll(e.id);
            any = true;
        }
    }
    if (!any)
        return false;
    m_entries.removeIf(drop);
    rebuildVisible();
    Q_EMIT threadPageChanged();
    Q_EMIT unseenCountChanged();
    return true;
}

void ActivityModel::eventRedacted(const QString &rawRoomId, const QString &eventId)
{
    if (eventId.isEmpty())
        return;
    const QString roomId = realRoomId(rawRoomId);
    if (!m_redactedIds.contains(eventId)) {
        m_redactedIds.insert(eventId);
        m_redactedOrder.append(eventId);
        while (m_redactedOrder.size() > kMaxRedactedIds)
            m_redactedIds.remove(m_redactedOrder.takeFirst());
    }
    // The row itself (a message, or a reaction by its own id), and any row
    // pointing at a redacted message: a reaction to it, an edit-made mention.
    // What the sender retracted must not stay readable in a preview.
    removeEntries([&](const Entry &e) {
        if (e.kind == QLatin1String("invite"))
            return false;
        if (!roomId.isEmpty() && e.roomId != roomId)
            return false;
        return e.id == eventId || e.eventId == eventId;
    });
}

void ActivityModel::dropSender(const QString &userId)
{
    if (userId.isEmpty())
        return;
    removeEntries([&userId](const Entry &e) { return e.senderId == userId; });
}

bool ActivityModel::noteReaction(const QString &rawRoomId, const QString &roomName,
                                 const QString &reactionEventId,
                                 const QString &targetEventId, const QString &senderId,
                                 const QString &senderName, const QString &key,
                                 qint64 timestampMs, bool targetIsOwn)
{
    if (!m_client || reactionEventId.isEmpty() || targetEventId.isEmpty())
        return false;
    if (senderId.isEmpty() || senderId == m_client->currentUserId())
        return false;
    if (isIgnored(senderId))
        return false;
    if (!targetIsOwn && !m_ownEventIds.contains(targetEventId))
        return false;
    if (m_redactedIds.contains(targetEventId))
        return false;
    const QString roomId = realRoomId(rawRoomId);
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
    e.eventId = targetEventId;
    return addEntry(std::move(e));
}

static bool inviteNameIsPlaceholder(const QString &name, const QString &roomId)
{
    return name.isEmpty() || name == roomId
        || name.startsWith(QLatin1String("Empty Room"));
}

bool ActivityModel::noteInvite(const RoomInfo &room)
{
    if (room.id.isEmpty() || room.membership != RoomInfo::Invited)
        return false;
    if (m_answeredInvites.contains(room.id))
        return false;
    if (isIgnored(room.inviterUserId))
        return false;
    Entry e;
    e.id = QStringLiteral("invite:") + room.id;
    if (m_ids.contains(e.id)) {
        for (int i = 0; i < m_entries.size(); ++i) {
            Entry &old = m_entries[i];
            if (old.id != e.id)
                continue;
            bool changed = false;
            if (old.roomNamePending
                && !inviteNameIsPlaceholder(room.name, room.id)) {
                old.roomName = room.name;
                old.roomNamePending = false;
                changed = true;
            }
            if (old.senderNamePending && !room.inviterUserId.isEmpty()) {
                const QString name = room.inviterDisplayName.isEmpty()
                    ? room.inviterUserId : room.inviterDisplayName;
                if (old.senderId != room.inviterUserId || old.senderName != name) {
                    old.senderId = room.inviterUserId;
                    old.senderName = name;
                    old.senderNamePending = name == room.inviterUserId;
                    changed = true;
                }
            }
            const int visibleRow = m_visible.indexOf(i);
            if (changed && visibleRow >= 0) {
                const QModelIndex idx = index(visibleRow, 0);
                Q_EMIT dataChanged(idx, idx, { RoomNameRole, SenderNameRole });
            }
            break;
        }
        return false;
    }
    e.kind = QStringLiteral("invite");
    e.roomId = room.id;
    e.roomName = room.name.isEmpty() ? room.id : room.name;
    e.roomNamePending = inviteNameIsPlaceholder(room.name, room.id);
    e.senderId = room.inviterUserId;
    e.senderName = room.inviterDisplayName.isEmpty() ? room.inviterUserId
                                                     : room.inviterDisplayName;
    e.senderNamePending = e.senderId.isEmpty() || e.senderName == e.senderId;
    const QVariantMap rec = m_invites.value(room.id).toMap();
    if (!rec.isEmpty()) {
        e.timestampMs = rec.value(QStringLiteral("t")).toLongLong();
        e.seenMark = rec.value(QStringLiteral("seen")).toBool();
    } else {
        e.timestampMs = now();
        if (m_storeLoaded)
            rememberInvite(room.id, e.timestampMs, false);
    }
    return addEntry(std::move(e));
}

void ActivityModel::inviteResolved(const QString &roomId)
{
    const QString id = QStringLiteral("invite:") + roomId;
    forgetInvite(roomId);
    if (!m_ids.contains(id))
        return;
    m_ids.remove(id);
    m_entries.removeIf([&id](const Entry &e) { return e.id == id; });
    rebuildVisible();
    Q_EMIT threadPageChanged();
    Q_EMIT unseenCountChanged();
}

void ActivityModel::inviteAnswered(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    if (m_answeredInvites.size() >= kMaxRememberedInvites)
        m_answeredInvites.clear();
    m_answeredInvites.insert(roomId);
    inviteResolved(roomId);
}

void ActivityModel::seed(const QVariantList &entries, bool firstPage)
{
    if (!m_client)
        return;
    const QString self = m_client->currentUserId();
    bool any = false;
    QStringList seeded;
    for (const QVariant &v : entries) {
        const QVariantMap m = v.toMap();
        if (firstPage && m.value(QStringLiteral("read")).toBool()) {
            const qint64 ts = m.value(QStringLiteral("timestampMs")).toLongLong();
            if (m_readHorizonDays > 0 && ts > 0
                && ts < now() - qint64(m_readHorizonDays) * 86400000LL)
                continue;
        }
        Entry e;
        e.id = m.value(QStringLiteral("eventId")).toString();
        e.roomId = m.value(QStringLiteral("roomId")).toString();
        e.senderId = m.value(QStringLiteral("senderId")).toString();
        // A row a forged edit listed under this id must not keep the real
        // original out; one the original confirms stays as it is.
        if (!e.id.isEmpty() && e.senderId != self)
            settleProvisional(e.id, e.senderId, e.roomId);
        if (e.id.isEmpty() || e.roomId.isEmpty() || m_ids.contains(e.id)
            || e.senderId == self || m_redactedIds.contains(e.id)
            || isIgnored(e.senderId))
            continue;
        e.kind = m.value(QStringLiteral("kind")).toString();
        if (!kKnownKinds.contains(e.kind))
            e.kind = QStringLiteral("highlight");
        e.roomName = m.value(QStringLiteral("roomName")).toString();
        if (e.roomName.isEmpty() && m_client)
            e.roomName = m_client->roomInfo(e.roomId).name;
        if (e.roomName.isEmpty() || e.roomName == e.roomId) {
            e.roomName = e.roomId;
            e.roomNamePending = true;
        }
        e.senderName = m.value(QStringLiteral("senderName")).toString();
        if (e.senderName.isEmpty() || e.senderName == e.senderId) {
            e.senderName = e.senderId;
            e.senderNamePending = true;
        }
        const QString glyph =
            glyphForMsgtype(m.value(QStringLiteral("msgtype")).toString());
        e.preview = !glyph.isEmpty()
            ? glyph
            : collapse(matrix::preview::stripReplyFallback(
                  m.value(QStringLiteral("preview")).toString()));
        e.encrypted = m.value(QStringLiteral("encrypted")).toBool();
        if (e.encrypted)
            e.preview.clear();
        e.highlight = true;
        e.arrivedMs = now();
        e.timestampMs = m.value(QStringLiteral("timestampMs")).toLongLong();
        e.eventId = e.id;
        e.threadRootId = m.value(QStringLiteral("threadRootId")).toString();
        // The server's read flag counts as seen, as in Element, but it is not
        // trusted alone; see reconcileSeedAgainstRoomCounts().
        e.seenMark = m.value(QStringLiteral("read")).toBool();
        e.readMark = e.seenMark;
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
    Q_EMIT threadPageChanged();
    m_seedAwaitingRooms += reconcileSeedAgainstRoomCounts(seeded);
    rebuildVisible();
    Q_EMIT unseenCountChanged();
}

QStringList ActivityModel::reconcileSeedAgainstRoomCounts(const QStringList &seededIds,
                                                          bool *flippedAny)
{
    if (flippedAny)
        *flippedAny = false;
    if (!m_client || seededIds.isEmpty())
        return {};
    const QSet<QString> seeded(seededIds.begin(), seededIds.end());

    QHash<QString, int> budget;
    QHash<QString, int> liveUnread;
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.highlight && !seeded.contains(e.id) && !isSeen(e)
            && e.kind != QLatin1String("invite"))
            ++liveUnread[e.roomId];
    }
    QSet<QString> unknownRooms;
    for (const Entry &e : m_entries) {
        if (!seeded.contains(e.id) || budget.contains(e.roomId))
            continue;
        const RoomInfo info = m_client->roomInfo(e.roomId);
        if (info.id != e.roomId) {
            unknownRooms.insert(e.roomId);
            continue;
        }
        budget.insert(e.roomId,
                      std::max(0, info.highlightCount - liveUnread.value(e.roomId)));
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

    for (Entry &e : m_entries) {
        if (e.seenMark || !seeded.contains(e.id))
            continue;
        auto it = budget.find(e.roomId);
        if (it == budget.end())
            continue;
        if (*it > 0) {
            --*it;
            continue;
        }
        e.seenMark = true;
        e.readMark = true;
        if (flippedAny)
            *flippedAny = true;
    }
    return deferred;
}

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
            if (info.id == e.roomId
                && !inviteNameIsPlaceholder(info.name, e.roomId)) {
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
        const int visibleRow = m_visible.indexOf(i);
        if (visibleRow >= 0) {
            const QModelIndex idx = index(visibleRow, 0);
            Q_EMIT dataChanged(idx, idx,
                               { RoomNameRole, SenderNameRole });
        }
    }
}

void ActivityModel::pruneRoomsNoLongerJoined()
{
    if (!m_client)
        return;
    for (auto it = m_answeredInvites.begin(); it != m_answeredInvites.end();) {
        const RoomInfo info = m_client->roomInfo(*it);
        if (info.id != *it || info.membership != RoomInfo::Invited)
            it = m_answeredInvites.erase(it);
        else
            ++it;
    }
    QSet<QString> held;
    for (const Entry &e : std::as_const(m_entries))
        if (e.kind != QLatin1String("invite") && !e.roomId.isEmpty())
            held.insert(e.roomId);
    for (auto it = m_roomMissingSince.begin(); it != m_roomMissingSince.end();) {
        if (!held.contains(it.key()))
            it = m_roomMissingSince.erase(it);
        else
            ++it;
    }
    const qint64 t = now();
    QSet<QString> gone;
    for (const QString &roomId : std::as_const(held)) {
        const RoomInfo info = m_client->roomInfo(roomId);
        const bool known = info.id == roomId;
        const bool present = known
            && (info.membership == RoomInfo::Joined
                || info.membership == RoomInfo::Invited);
        if (present) {
            m_roomsEverKnown.insert(roomId);
            m_roomMissingSince.remove(roomId);
            continue;
        }
        if (known && info.membership == RoomInfo::Left) {
            gone.insert(roomId);
            continue;
        }
        const bool everKnown = m_roomsEverKnown.contains(roomId);
        if (!everKnown && !m_client->initialSyncDone())
            continue;
        auto since = m_roomMissingSince.find(roomId);
        if (since == m_roomMissingSince.end()) {
            m_roomMissingSince.insert(roomId, t);
            continue;
        }
        const qint64 grace = everKnown ? kLeftRoomGraceMs : kUnknownRoomGraceMs;
        if (t - *since >= grace)
            gone.insert(roomId);
    }
    if (gone.isEmpty())
        return;
    for (const QString &roomId : std::as_const(gone)) {
        m_roomMissingSince.remove(roomId);
        m_roomsEverKnown.remove(roomId);
    }
    removeEntries([&gone](const Entry &e) {
        return e.kind != QLatin1String("invite") && gone.contains(e.roomId);
    });
}

bool ActivityModel::addEntry(Entry entry)
{
    if (m_ids.contains(entry.id) || m_redactedIds.contains(entry.id))
        return false;
    m_ids.insert(entry.id);
    if (entry.arrivedMs <= 0)
        entry.arrivedMs = now();
    int pos = 0;
    while (pos < m_entries.size() && m_entries.at(pos).timestampMs > entry.timestampMs)
        ++pos;
    const bool visible = passesFilter(entry);
    m_entries.insert(pos, std::move(entry));
    if (visible) {
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
    Q_EMIT threadPageChanged();
    Q_EMIT unseenCountChanged();
    ++m_threadActivityRevision;
    Q_EMIT threadActivityChanged();
    return true;
}
