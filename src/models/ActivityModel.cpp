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
// A room id as the logs print it (matrix::e2ee::redactId's form: the sigil,
// then 8 hex of SHA-256 of the rest), without linking the crypto helpers into
// every test that builds this model.
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

// Every kind a row may claim. "highlight" is what the server gives without
// saying which push rule matched, so it claims only that.
const QStringList kKnownKinds{
    QStringLiteral("mention"),      QStringLiteral("room_mention"),
    QStringLiteral("reply"),        QStringLiteral("thread"),
    QStringLiteral("reaction"),     QStringLiteral("invite"),
    QStringLiteral("keyword"),      QStringLiteral("highlight"),
};
constexpr int kMaxOwnEventIds = 2048;
constexpr int kMaxRedactedIds = 512;

// Kinds that are highlights in the server's sense (highlight_count).
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
    // The account's store loads as soon as the account is known, not on the
    // first Syncing: an offline restore never reaches Syncing, and anything
    // written before the load (a seen marker, a keyword) would replace the
    // account's real state. Deferred one turn so every direct handler of
    // loginSucceeded, including the one that makes this account the active
    // one in settings, has run. Syncing stays as a fallback.
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
    // Only a failure retrying cannot fix sets the hint; a dropped connection
    // leaves it as it was, and any answer clears it.
    connect(m_client, &MatrixClient::activitySeedFailed, this,
            [this](const QString &category) {
        if (seedFailureIsPermanent(category))
            setHistoryUnavailable(true);
    });
    connect(m_client, &MatrixClient::activitySeedReceived, this,
            [this] { setHistoryUnavailable(false); });
    // A left, kicked or banned room's rows go once it has stayed gone.
    connect(m_client, &MatrixClient::roomsChanged, this,
            &ActivityModel::pruneRoomsNoLongerJoined);
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
    // No account, no store: the settings key is derived from the active user.
    if (!m_client || m_client->currentUserId().isEmpty())
        return;
    m_storeLoaded = true;
    const QVariantMap saved = m_store.load();
    const qint64 stored = saved.value(QStringLiteral("seenUpToMs")).toLongLong();
    m_seenLoadedUpToMs = std::max<qint64>(0, stored);
    // A "Mark all seen" before the load (offline, early) is kept, not lost.
    const bool markerMoved = m_seenUpToMs > m_seenLoadedUpToMs;
    m_seenUpToMs = std::max(m_seenUpToMs, m_seenLoadedUpToMs);
    // Pending invites this account already noted: their first-noted time and
    // seen state, applied to rows that arrived before the load as well.
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
    // Bounded: the oldest go first. An invite answered while the app was
    // closed is never resolved here, so the bound is what retires it.
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

// Seen state of invite rows is the one per-row mark that persists. Records
// it without saving; true when the caller must save.
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
    const bool keywordsMoved = !m_keywords.isEmpty();
    m_keywords.clear();
    endResetModel();
    if (keywordsMoved)
        Q_EMIT keywordsChanged();
    // The next account's server may have the endpoint.
    setHistoryUnavailable(false);
    Q_EMIT countChanged();
    Q_EMIT unseenCountChanged();
}

void ActivityModel::setHistoryUnavailable(bool unavailable)
{
    if (m_historyUnavailable == unavailable)
        return;
    m_historyUnavailable = unavailable;
    Q_EMIT historyUnavailableChanged();
}

bool ActivityModel::seedFailureIsPermanent(const QString &category)
{
    // http_failure_reason (rust/src/oauth.rs): "http_<status>_<errcode>", or a
    // transport token. The spec's answer for an endpoint a server does not
    // implement is M_UNRECOGNIZED (404 or 405); a proxy in front of it may give
    // the bare status. Everything else (a timeout, a 5xx, a rate limit) is
    // worth asking again.
    if (category.endsWith(QLatin1String("_M_UNRECOGNIZED")))
        return true;
    return category.startsWith(QLatin1String("http_404_"))
        || category.startsWith(QLatin1String("http_405_"))
        || category.startsWith(QLatin1String("http_501_"));
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
    // The loaded marker, not the live one: a row present at this session's
    // "Mark all seen" carries seenMark, and one arriving after it has not been
    // seen whatever its origin_server_ts says.
    return e.seenMark
        || (e.timestampMs > 0 && e.timestampMs <= m_seenLoadedUpToMs);
}

bool ActivityModel::isMutedRoom(const Entry &e) const
{
    // An invite names a room the user is not in yet, which has no mode.
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

int ActivityModel::unseenCount() const
{
    int n = 0;
    for (const Entry &e : m_entries)
        if (!isSeen(e) && !isMutedRoom(e))
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
        if (e.seenMark || e.roomId != roomId)
            continue;
        if (e.kind == QLatin1String("invite"))
            continue;
        // A threaded receipt covers its own thread only; an unthreaded one
        // covers the room, threads included.
        if (!threadRootId.isEmpty() && e.threadRootId != threadRootId)
            continue;
        // A reaction is not a timeline row, so the receipt never lands on it
        // and its own timestamp is usually newer than the receipt (a reaction
        // to an older message). Reading the room AFTER the reaction arrived
        // has shown it, on the message it decorates.
        if (e.kind == QLatin1String("reaction") && threadRootId.isEmpty()
            && e.arrivedMs > 0 && e.arrivedMs < readAtMs) {
            e.seenMark = true;
            changed = true;
            continue;
        }
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
    // The SDK's client-side counts skip thread events and an encrypted room
    // has no server count to fall back on, so a clear encrypted room says
    // nothing about its threads. Fail closed while encryption is unknown.
    const bool countsCoverThreads = !info.encrypted && info.encryptionKnown;
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
    int marked = 0;
    qint64 nearestLagMs = -1;
    for (Entry &e : m_entries) {
        if (e.seenMark || e.roomId != roomId)
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
        e.seenMark = true;
        ++marked;
        const qint64 lagMs = info.countsWrittenAtMs - e.arrivedMs;
        if (nearestLagMs < 0 || lagMs < nearestLagMs)
            nearestLagMs = lagMs;
    }
    // The one path that marks rows seen without the user acting here. Counts,
    // a delay and a hashed room only, so a row reported "seen before I opened
    // it" (break-it rerun, section 10, F4: NOT explained) can be traced to the
    // payload that did it. The open candidate it settles: a room payload
    // written just AFTER the row arrived but before matrix-sdk's event-cache
    // task had counted the event (it processes a sync's updates
    // asynchronously from the event handlers), so its client counts were
    // still clear; in an unencrypted room that also needs the server's counts
    // to have been absent from that response. A small counts_after_row_ms with
    // no "timeline open" or receipt for the room nearby points there.
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
    if (isMutedRoom(e))
        return false;
    if (m_filter == QLatin1String("all"))
        return true;
    // "highlight" is a push-rule highlight the server did not explain (every
    // seeded row, a legacy display-name mention, a server keyword): most of
    // them are mentions, and leaving them out emptied this tab after every
    // restart.
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
    // An edit is not a new message: only a user it NEWLY mentions (an edit's
    // m.mentions lists exactly those) is told. Its keyword or reply relation
    // was the original's, already classified.
    if (event.isEdit)
        return event.mentionsMe ? QStringLiteral("mention") : QString();
    // A reply first: clients that follow intentional mentions also put the
    // replied-to user in m.mentions, which would otherwise make every reply a
    // "mention" and leave the Replies tab empty.
    if ((!event.replyToSenderId.isEmpty() && event.replyToSenderId == selfUserId)
        || (!event.replyToEventId.isEmpty() && ownEventIds.contains(event.replyToEventId)))
        return QStringLiteral("reply");
    if (event.mentionsMe)
        return QStringLiteral("mention");
    // The user's thread: one they replied in, or one rooted at their own
    // message. The latter is not a thread root yet when it is sent, so the
    // own-message set is asked directly.
    if (!event.threadRootId.isEmpty()
        && (ownThreadRoots.contains(event.threadRootId)
            || ownEventIds.contains(event.threadRootId)))
        return QStringLiteral("thread");
    if (event.pushVerdict != TimelineEvent::PushVerdict::Unknown) {
        // The account's push rules decided. A highlight that is not a direct
        // mention is a server keyword or a legacy display-name mention;
        // m.mentions.room counts only when the rules highlighted it, since
        // .m.rule.is_room_mention also checks the sender's power to notify
        // the room, which any member can otherwise skip.
        if (!event.pushHighlight)
            return {};
        return event.mentionsRoom ? QStringLiteral("room_mention")
                                  : QStringLiteral("highlight");
    }
    // No verdict (a backend that evaluates no push rules): the bell's legacy
    // keyword list and the raw @room flag are all there is.
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

// The same glyphs previewOf() gives a live row, keyed on the seed's msgtype.
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
    // A thread the user started or replied in is "their" thread.
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
        // An own edit is not a message anyone reacts or replies to.
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
    e.highlight = isHighlightKind(kind) || event.mentionsMe || event.pushHighlight;
    e.roomId = event.roomId;
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

// An edit refreshes the row of the message it replaces and never adds one of
// its own; each edit has a fresh id, so treating it as a message listed one
// row per typo fix, with the "* " fallback as its text.
bool ActivityModel::ingestEdit(const TimelineEvent &event, const QString &roomName)
{
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
        // Newly mentioned by the edit: the row now says so.
        if (event.mentionsMe && e.kind != QLatin1String("mention")
            && e.kind != QLatin1String("reply")) {
            e.kind = QStringLiteral("mention");
            e.highlight = true;
            roles << KindRole;
        }
        if (roles.isEmpty())
            return false;
        if (roles.contains(KindRole)) {
            // The kind decides the filter, so visibility can move.
            rebuildVisible();
        } else {
            const int row = m_visible.indexOf(i);
            if (row >= 0)
                Q_EMIT dataChanged(index(row), index(row), roles);
        }
        return false;
    }
    if (!event.mentionsMe || isIgnored(event.sender))
        return false;
    // Newly mentioned in a message this list never held: one row, for the
    // ORIGINAL message, so later edits update it instead of adding more.
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
    // A row an edit listed: the original confirms it (and its text is older
    // than the edit's) or, when another sender forged it, takes its place.
    if (settleProvisional(event.eventId, event.sender, event.roomId))
        return;
    const QString kind =
        classify(event, self, m_ownEventIds, m_ownThreadRoots, m_keywords);
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.id != event.eventId || e.kind == QLatin1String("reaction"))
            continue;
        // Replace in place under the same id: never a second row. Seen state
        // and arrival stay; what the ciphertext hid is filled in.
        const bool kindMoved = !kind.isEmpty() && kind != e.kind;
        if (kindMoved)
            e.kind = kind;
        e.highlight = e.highlight || isHighlightKind(kind) || event.mentionsMe
            || event.pushHighlight;
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
    // The backend's word for a target this session never saw sent (an older
    // message, one sent from another device), or the session's own memory.
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
    e.eventId = targetEventId;   // navigate to the message that was reacted to
    return addEntry(std::move(e));
}

// The name an unsettled stripped invite carries: empty, the raw id, or the
// SDK's own "Empty Room" placeholder (also "Empty Room (was X)"). Non-empty,
// so it used to freeze the row; none of them is a name worth keeping.
static bool inviteNameIsPlaceholder(const QString &name, const QString &roomId)
{
    return name.isEmpty() || name == roomId
        || name.startsWith(QLatin1String("Empty Room"));
}

bool ActivityModel::noteInvite(const RoomInfo &room)
{
    if (room.id.isEmpty() || room.membership != RoomInfo::Invited)
        return false;
    // Answered here; the list has not caught up yet (see inviteAnswered).
    if (m_answeredInvites.contains(room.id))
        return false;
    if (isIgnored(room.inviterUserId))
        return false;
    Entry e;
    e.id = QStringLiteral("invite:") + room.id;
    if (m_ids.contains(e.id)) {
        // Seen again: the first sighting is often unsettled (placeholder
        // name, no invite details yet), so a row still pending takes what the
        // room list reports now. Rows only move away from a placeholder.
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
    // Invites are where a raw id is most likely to stick (no roster is fetched
    // for a room the user is not in); the roomsChanged sweep can still name the
    // room from the invite payload.
    e.senderNamePending = e.senderId.isEmpty() || e.senderName == e.senderId;
    // Stripped invite state carries no origin_server_ts. Stamping "now" on
    // every launch made a two-week-old invite the newest, unseen row after
    // each restart, so the first time this account noted it is remembered,
    // with its seen state. Before the store loads, loadStore() corrects it.
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
    Q_EMIT unseenCountChanged();
}

void ActivityModel::inviteAnswered(const QString &roomId)
{
    if (roomId.isEmpty())
        return;
    // Bounded like the remembered invites; a set this size is only ever
    // reached by a burst of answers the list has not caught up with.
    if (m_answeredInvites.size() >= kMaxRememberedInvites)
        m_answeredInvites.clear();
    m_answeredInvites.insert(roomId);
    inviteResolved(roomId);
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
        // A row a forged edit listed under this id must not keep the real
        // original out; one the original confirms stays as it is.
        if (!e.id.isEmpty() && e.senderId != self)
            settleProvisional(e.id, e.senderId, e.roomId);
        if (e.id.isEmpty() || e.roomId.isEmpty() || m_ids.contains(e.id)
            || e.senderId == self || m_redactedIds.contains(e.id)
            || isIgnored(e.senderId))
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
        // The same text a live row shows: the reply fallback quote stripped,
        // and a media message as its glyph rather than its filename.
        const QString glyph =
            glyphForMsgtype(m.value(QStringLiteral("msgtype")).toString());
        e.preview = !glyph.isEmpty()
            ? glyph
            : collapse(matrix::preview::stripReplyFallback(
                  m.value(QStringLiteral("preview")).toString()));
        e.encrypted = m.value(QStringLiteral("encrypted")).toBool();
        if (e.encrypted)
            e.preview.clear();
        // Every seeded row is a server highlight (only=highlight).
        e.highlight = true;
        e.arrivedMs = now();
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
    // Unread highlights already listed from live ingestion (the initial-sync
    // window delivers the newest ones before the seed lands; the seed skips
    // their ids). They are the room's unread highlights too, so they spend
    // the budget first, or the bell counts more than the room list.
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
            // Unknown room: leave its rows to the server's flag for now and
            // return them to the caller. A room is reconciled all-or-nothing,
            // so a retry cannot double-spend its budget.
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

void ActivityModel::pruneRoomsNoLongerJoined()
{
    if (!m_client)
        return;
    // An answered invite is forgotten once the list no longer reports it as
    // an invite (joined, left, or gone): from then on a new invite to that
    // room is a new invite.
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
    // Forget rooms that no longer hold rows.
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
        // The room list carries joined, invited and knocked rooms; a leave,
        // kick or ban removes the room from it.
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
        // A room never seen (a seeded row) counts as missing only once the
        // room list has loaded.
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
