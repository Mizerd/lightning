#include "models/MessageSearchController.h"

#include "matrix/MatrixClient.h"

namespace {
constexpr int kDebounceMs = 400;
constexpr int kPageSize = 25;
constexpr int kMaxFilteredPagesPerAction = 4;

QSet<QString> stringSet(const QVariant &value)
{
    QSet<QString> out;
    const QVariantList values = value.toList();
    for (const QVariant &entry : values) {
        const QString text = entry.toString().trimmed();
        if (!text.isEmpty())
            out.insert(text);
    }
    return out;
}
} // namespace

MessageSearchController::MessageSearchController(QObject *parent)
    : QAbstractListModel(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this,
            [this] { dispatch(/*nextPage=*/false); });
}

void MessageSearchController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clear();
    if (!m_client) {
        Q_EMIT stateChanged();
        return;
    }
    connect(m_client, &MatrixClient::messageSearchFinished, this,
            &MessageSearchController::onSearchFinished);
    connect(m_client, &MatrixClient::localSearchFinished, this,
            &MessageSearchController::onLocalSearchFinished);
    connect(m_client, &MatrixClient::searchIndexStatsReceived, this,
            [this](quint64, qint64 messages, qint64 rooms) {
        if (m_indexedMessages == messages && m_indexedRooms == rooms)
            return;
        m_indexedMessages = messages;
        m_indexedRooms = rooms;
        Q_EMIT indexStatsChanged();
    });
    connect(m_client, &MatrixClient::searchIndexSwept, this,
            [this](quint64, int, int, qint64 messages, qint64 rooms) {
        // The periodic sweep can index a message while results are on screen;
        // re-run the query only when the index actually grew.
        const bool grew = messages > m_indexedMessages;
        m_indexedMessages = messages;
        m_indexedRooms = rooms;
        Q_EMIT indexStatsChanged();
        if (grew && !m_query.trimmed().isEmpty()
            && effectiveSource() == QLatin1String("local")) {
            dispatch(false);
        }
    });
    connect(m_client, &MatrixClient::searchIndexDeepened, this,
            [this](quint64 opId, bool ok, const QString &roomId, int,
                   bool reachedStart, int written, qint64 messages,
                   const QString &) {
        if (opId != m_deepOp)
            return;   // a deep index from a previous room or account
        m_deepOp = 0;
        m_indexedMessages = messages;
        Q_EMIT indexingChanged();
        Q_EMIT indexStatsChanged();
        Q_EMIT roomHistoryIndexed(roomId, ok, reachedStart, written);
        // Re-run the query against what arrived. Not gated on `written`: the
        // user asked, and the periodic sweep may already have indexed the new
        // message.
        if (ok && !m_query.trimmed().isEmpty()
            && effectiveSource() == QLatin1String("local")) {
            dispatch(false);
        }
    });
    // One account's search results must never surface under another's.
    connect(m_client, &MatrixClient::loggedOut, this,
            &MessageSearchController::clear);
    Q_EMIT stateChanged();
}

QString MessageSearchController::effectiveSource() const
{
    // "local" is preferred where it exists (works in encrypted rooms, no round
    // trip), but a backend without an index must fall back rather than go dead.
    if (m_source == QLatin1String("local") && m_client
        && m_client->supportsLocalSearch()) {
        return QStringLiteral("local");
    }
    return QStringLiteral("server");
}

bool MessageSearchController::supported() const
{
    if (!m_client)
        return false;
    // Local search works wherever the index does, including encrypted rooms;
    // server search only where the server can read the room.
    if (effectiveSource() == QLatin1String("local"))
        return true;
    return m_client->supportsMessageSearch();
}

bool MessageSearchController::localAvailable() const
{
    return m_client && m_client->supportsLocalSearch();
}

void MessageSearchController::setSource(const QString &source)
{
    // Validated against the known set: an unknown value would leave dispatch()
    // choosing neither branch.
    const QString next = source == QLatin1String("server")
        ? QStringLiteral("server") : QStringLiteral("local");
    if (next == m_source)
        return;
    m_source = next;
    invalidatePending();
    clear();
    Q_EMIT sourceChanged();
    Q_EMIT stateChanged();
    if (!m_query.trimmed().isEmpty())
        dispatch(false);
}

void MessageSearchController::refreshIndexStats()
{
    if (m_client)
        m_client->searchIndexStats();
}

void MessageSearchController::sweepIndex()
{
    if (m_client)
        m_client->sweepSearchIndex();
}

void MessageSearchController::indexRoomHistory(const QString &roomId)
{
    if (!m_client || roomId.isEmpty() || m_deepOp != 0)
        return;   // one at a time: each is a bounded run of real requests
    m_deepOp = m_client->deepenSearchIndex(roomId);
    if (m_deepOp != 0)
        Q_EMIT indexingChanged();
}

void MessageSearchController::clearIndex()
{
    if (!m_client)
        return;
    m_client->clearSearchIndex();
    m_indexedMessages = 0;
    m_indexedRooms = 0;
    Q_EMIT indexStatsChanged();
    if (effectiveSource() == QLatin1String("local"))
        clear();
}

void MessageSearchController::onLocalSearchFinished(
    quint64 opId, bool ok, const QString &category, int minChars,
    const QVariantList &results)
{
    if (opId != m_pendingOp)
        return;   // stale: the query moved on while this was in flight
    m_pendingOp = 0;
    // What this page asked for. Local paging re-runs the query with a bigger
    // limit and replaces the rows, so the exhaustion test compares against
    // this.
    const int requested = m_pendingLocalLimit > 0 ? m_pendingLocalLimit
                                                  : kLocalPage;
    m_pendingLocalLimit = 0;
    // Raw size of this page, used for the next request's limit.
    m_lastLocalRawCount = static_cast<int>(results.size());
    if (minChars > 0 && minChars != m_minLocalChars)
        m_minLocalChars = minChars;

    if (!ok) {
        // "too_short" is neither an error nor "no results": the query cannot
        // match the tokenizer, and one more character fixes it.
        beginResetModel();
        m_rows.clear();
        endResetModel();
        m_totalCount = 0;
        m_nextBatch.clear();
        setState(category == QLatin1String("too_short")
                     ? QStringLiteral("too_short") : QStringLiteral("error"));
        return;
    }

    QList<QVariantMap> rows;
    rows.reserve(results.size());
    for (const QVariant &value : results) {
        QVariantMap row = value.toMap();
        // Same client-side filters as the server path, so switching source does
        // not change which rows a filter removes.
        if (!matchesFilters(row))
            continue;
        if (m_client) {
            const QString roomId = row.value(QStringLiteral("roomId")).toString();
            row.insert(QStringLiteral("roomName"),
                       m_client->roomInfo(roomId).name);
        }
        rows.append(row);
    }

    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    m_totalCount = static_cast<quint64>(m_rows.size());
    // No cursor: more exists only if the page came back full, where full means
    // the limit this request asked for (not kLocalPage), otherwise canLoadMore
    // never clears and loadMore spins. Counted in raw results, before
    // matchesFilters().
    m_nextBatch = results.size() >= requested
        ? QStringLiteral("local") : QString();
    setState(m_rows.isEmpty() ? QStringLiteral("no_results")
                              : QStringLiteral("results"));
}

void MessageSearchController::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();
    invalidatePending();
    if (m_query.trimmed().isEmpty()) {
        m_debounce.stop();
        clear();
        return;
    }
    m_debounce.start();
}

void MessageSearchController::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    Q_EMIT roomIdChanged();
    // A different scope answers a different question: drop the old results.
    clear();
}

void MessageSearchController::setFilters(const QVariantMap &filters)
{
    if (m_filters == filters)
        return;
    m_filters = filters;
    rebuildFilterSets();
    Q_EMIT filtersChanged();
    // Applied criteria define a new result set; searching explicitly keeps a
    // multi-control Apply to one request.
    clear();
}

int MessageSearchController::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant MessageSearchController::data(const QModelIndex &index,
                                       int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const QVariantMap &row = m_rows.at(index.row());
    switch (role) {
    case RoomIdRole: return row.value(QStringLiteral("roomId"));
    case RoomNameRole: return row.value(QStringLiteral("roomName"));
    case EventIdRole: return row.value(QStringLiteral("eventId"));
    case SenderRole: return row.value(QStringLiteral("sender"));
    case SenderDisplayNameRole:
        return row.value(QStringLiteral("senderDisplayName"));
    case SenderAvatarUrlRole:
        return row.value(QStringLiteral("senderAvatarUrl"));
    case TimestampMsRole: return row.value(QStringLiteral("timestampMs"));
    case MsgtypeRole: return row.value(QStringLiteral("msgtype"));
    case BodyRole: return row.value(QStringLiteral("body"));
    }
    return {};
}

QHash<int, QByteArray> MessageSearchController::roleNames() const
{
    return {
        { RoomIdRole, QByteArrayLiteral("roomId") },
        { RoomNameRole, QByteArrayLiteral("roomName") },
        { EventIdRole, QByteArrayLiteral("eventId") },
        { SenderRole, QByteArrayLiteral("sender") },
        { SenderDisplayNameRole, QByteArrayLiteral("senderDisplayName") },
        { SenderAvatarUrlRole, QByteArrayLiteral("senderAvatarUrl") },
        { TimestampMsRole, QByteArrayLiteral("timestampMs") },
        { MsgtypeRole, QByteArrayLiteral("msgtype") },
        { BodyRole, QByteArrayLiteral("body") },
    };
}

void MessageSearchController::search()
{
    m_debounce.stop();
    if (m_query.trimmed().isEmpty())
        return;
    dispatch(/*nextPage=*/false);
}

void MessageSearchController::loadMore()
{
    if (m_pendingOp != 0 || m_nextBatch.isEmpty())
        return;
    dispatch(/*nextPage=*/true);
}

void MessageSearchController::clear()
{
    m_debounce.stop();
    invalidatePending();
    beginResetModel();
    m_rows.clear();
    endResetModel();
    m_nextBatch.clear();
    m_totalCount = 0;
    setState(QStringLiteral("idle"));
}

QVariantMap MessageSearchController::rowAt(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    return m_rows.at(row);
}

void MessageSearchController::dispatch(bool nextPage)
{
    if (!supported() || m_query.trimmed().isEmpty())
        return;
    m_scanPages = 0;
    m_scanTarget = (nextPage ? m_rows.size() : 0) + kPageSize;
    requestPage(nextPage);
}

void MessageSearchController::requestPage(bool append)
{
    if (effectiveSource() == QLatin1String("local")) {
        // No cursor: request a bigger page and replace. Offset paging against
        // an index being written would drop or repeat rows. Grown from the last
        // raw page, not from filtered m_rows, or a fully filtered page would
        // freeze the limit.
        const int limit = append ? m_lastLocalRawCount + kLocalPage : kLocalPage;
        const quint64 opId = m_client->localSearch(m_query.trimmed(), m_roomId,
                                                   limit, 0);
        if (opId == 0) {
            setState(QStringLiteral("error"));
            return;
        }
        // The completion compares what came back against this request's limit.
        m_pendingLocalLimit = limit;
        m_pendingOp = opId;
        m_pendingIsNextPage = append;
        setState(append ? QStringLiteral("loading_more")
                        : QStringLiteral("loading"));
        return;
    }
    const QString since = append ? m_nextBatch : QString();
    const quint64 opId = m_client->searchMessages(m_query.trimmed(), m_roomId,
                                                  since, kPageSize,
                                                  m_filters);
    if (opId == 0) {
        setState(QStringLiteral("error"));
        return;
    }
    m_pendingLocalLimit = 0; // server paging carries a cursor, not a limit
    m_pendingOp = opId;
    m_pendingIsNextPage = append;
    ++m_scanPages;
    setState(append ? QStringLiteral("loading_more")
                    : QStringLiteral("loading"));
}

void MessageSearchController::onSearchFinished(quint64 opId, bool ok,
                                               const QVariantList &results,
                                               const QString &nextBatch,
                                               quint64 count,
                                               const QString &category)
{
    Q_UNUSED(category);
    if (opId == 0 || opId != m_pendingOp)
        return; // superseded — stale pages never repaint a newer query
    m_pendingOp = 0;
    if (!ok) {
        if (!m_pendingIsNextPage) {
            beginResetModel();
            m_rows.clear();
            endResetModel();
            m_nextBatch.clear();
        }
        setState(QStringLiteral("error"));
        return;
    }
    // Room names resolve locally; the account is in every room searched.
    QList<QVariantMap> rows;
    rows.reserve(results.size());
    for (const QVariant &value : results) {
        QVariantMap row = value.toMap();
        if (!matchesFilters(row))
            continue;
        const QString roomId = row.value(QStringLiteral("roomId")).toString();
        QString roomName;
        if (m_client)
            roomName = m_client->roomInfo(roomId).name;
        row.insert(QStringLiteral("roomName"),
                   roomName.isEmpty() ? roomId : roomName);
        rows.append(row);
    }
    if (!m_pendingIsNextPage) {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    } else if (!rows.isEmpty()) {
        beginInsertRows({}, m_rows.size(), m_rows.size() + rows.size() - 1);
        m_rows.append(rows);
        endInsertRows();
    }
    m_nextBatch = nextBatch;
    m_totalCount = count;
    // The Matrix search API cannot express mentions, dates, content kinds,
    // links or pinned state. Scan a small bounded number of server pages so a
    // filtered page is useful without walking whole histories.
    const bool hasClientFilters = !m_mentionUsers.isEmpty()
        || !m_contentTypes.isEmpty()
        || m_pinnedMode == QLatin1String("pinned")
        || m_pinnedMode == QLatin1String("not_pinned")
        || m_afterMs > 0 || m_beforeMs > 0;
    if (hasClientFilters && m_rows.size() < m_scanTarget
        && !m_nextBatch.isEmpty()
        && m_scanPages < kMaxFilteredPagesPerAction) {
        requestPage(/*append=*/true);
        return;
    }
    setState(m_rows.isEmpty() ? QStringLiteral("no_results")
                              : QStringLiteral("results"));
}

void MessageSearchController::setState(const QString &state)
{
    if (m_state == state) {
        Q_EMIT stateChanged(); // count/canLoadMore/totalCount ride this
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}

void MessageSearchController::invalidatePending()
{
    m_pendingOp = 0;
    m_pendingIsNextPage = false;
    m_pendingLocalLimit = 0;
    m_lastLocalRawCount = 0;
    m_nextBatch.clear();
}

void MessageSearchController::rebuildFilterSets()
{
    m_fromUsers = stringSet(m_filters.value(QStringLiteral("fromUserIds")));
    m_mentionUsers =
        stringSet(m_filters.value(QStringLiteral("mentionUserIds")));
    m_contentTypes =
        stringSet(m_filters.value(QStringLiteral("contentTypes")));
    m_pinnedEventIds =
        stringSet(m_filters.value(QStringLiteral("pinnedEventIds")));
    m_afterMs = m_filters.value(QStringLiteral("afterMs")).toLongLong();
    m_beforeMs = m_filters.value(QStringLiteral("beforeMs")).toLongLong();
    m_pinnedMode = m_filters.value(QStringLiteral("pinnedMode")).toString();
}

bool MessageSearchController::matchesFilters(const QVariantMap &row) const
{
    if (!m_fromUsers.isEmpty()
        && !m_fromUsers.contains(row.value(QStringLiteral("sender")).toString()))
        return false;

    if (!m_mentionUsers.isEmpty()) {
        bool mentioned = false;
        const QVariantList mentions =
            row.value(QStringLiteral("mentionUserIds")).toList();
        for (const QVariant &mention : mentions) {
            if (m_mentionUsers.contains(mention.toString())) {
                mentioned = true;
                break;
            }
        }
        if (!mentioned)
            return false;
    }

    const qint64 timestamp =
        row.value(QStringLiteral("timestampMs")).toLongLong();
    if (m_afterMs > 0 && timestamp < m_afterMs)
        return false;
    if (m_beforeMs > 0 && timestamp >= m_beforeMs)
        return false;

    if (!m_contentTypes.isEmpty()) {
        const QString msgtype =
            row.value(QStringLiteral("msgtype")).toString();
        bool kindMatch = false;
        if (m_contentTypes.contains(QStringLiteral("image"))
            && msgtype == QLatin1String("m.image"))
            kindMatch = true;
        if (m_contentTypes.contains(QStringLiteral("video"))
            && msgtype == QLatin1String("m.video"))
            kindMatch = true;
        if (m_contentTypes.contains(QStringLiteral("audio"))
            && msgtype == QLatin1String("m.audio"))
            kindMatch = true;
        if (m_contentTypes.contains(QStringLiteral("file"))
            && msgtype == QLatin1String("m.file"))
            kindMatch = true;
        if (m_contentTypes.contains(QStringLiteral("sticker"))
            && row.value(QStringLiteral("isSticker")).toBool())
            kindMatch = true;
        if (m_contentTypes.contains(QStringLiteral("link"))
            && row.value(QStringLiteral("hasLink")).toBool())
            kindMatch = true;
        if (!kindMatch)
            return false;
    }

    if (m_pinnedMode == QLatin1String("pinned")
        && !m_pinnedEventIds.contains(
            row.value(QStringLiteral("eventId")).toString()))
        return false;
    if (m_pinnedMode == QLatin1String("not_pinned")
        && m_pinnedEventIds.contains(
            row.value(QStringLiteral("eventId")).toString()))
        return false;
    return true;
}
