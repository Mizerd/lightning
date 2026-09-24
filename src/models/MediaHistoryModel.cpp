#include "models/MediaHistoryModel.h"

#include <algorithm>

#include <QLocale>
#include <QSet>
#include <QUrl>

namespace {
/// Events per page. The walk is unfiltered, so a page may add no rows in a
/// chatty room; loadMore() keeps going.
constexpr int kPageSize = 60;

/// Newest first, stable: attachments in one message share a timestamp and
/// must not reorder between pages.
bool newerFirst(qint64 a, qint64 b) { return a > b; }
} // namespace

MediaHistoryModel::MediaHistoryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void MediaHistoryModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    clearAll();
    if (m_client) {
        connect(m_client, &MatrixClient::mediaHistoryPage,
                this, &MediaHistoryModel::onPage);
        connect(m_client, &MatrixClient::mediaHistoryFailed,
                this, &MediaHistoryModel::onFailed);
        // A browse belongs to the account that opened it; clear on sign-out.
        connect(m_client, &MatrixClient::loggedOut, this,
                [this] {
                    m_roomId.clear();
                    clearAll();
                    Q_EMIT roomIdChanged();
                    Q_EMIT availableChanged();
                });
        // `available` depends on a live Rust handle, which appears at login,
        // well after setClient(); re-announce it then.
        connect(m_client, &MatrixClient::loginSucceeded, this,
                [this] { Q_EMIT availableChanged(); });
    }
    Q_EMIT availableChanged();
}

bool MediaHistoryModel::available() const
{
    return m_client && m_client->supportsMediaHistory();
}

int MediaHistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_shown.size());
}

QHash<int, QByteArray> MediaHistoryModel::roleNames() const
{
    return {
        { EventIdRole, "eventId" },
        { SenderRole, "sender" },
        { TimestampMsRole, "timestampMs" },
        { KindRole, "kind" },
        { BodyRole, "body" },
        { FilenameRole, "filename" },
        { MimetypeRole, "mimetype" },
        { SizeRole, "size" },
        { WidthRole, "width" },
        { HeightRole, "height" },
        { DurationMsRole, "durationMs" },
        { MxcRole, "mxc" },
        { ThumbnailMxcRole, "thumbnailMxc" },
        { MediaKeyRole, "mediaKey" },
        { EncryptedRole, "encrypted" },
        { UrlRole, "url" },
        { HostRole, "host" },
        { DateGroupRole, "dateGroup" },
    };
}

QVariant MediaHistoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_shown.size())
        return {};
    const Entry &e = m_all.at(m_shown.at(index.row()));
    switch (role) {
    case EventIdRole: return e.eventId;
    case SenderRole: return e.sender;
    case TimestampMsRole: return e.timestampMs;
    case KindRole: return e.kind;
    case BodyRole: return e.body;
    case FilenameRole: return e.filename;
    case MimetypeRole: return e.mimetype;
    case SizeRole: return e.size;
    case WidthRole: return e.width;
    case HeightRole: return e.height;
    case DurationMsRole: return e.durationMs;
    case MxcRole: return e.mxc;
    case ThumbnailMxcRole: return e.thumbnailMxc;
    case MediaKeyRole: return e.mediaKey;
    case EncryptedRole: return e.encrypted;
    case UrlRole: return e.url;
    case HostRole: return e.host;
    case DateGroupRole: {
        // Resolved in the viewer's time zone and locale: "Yesterday" means the
        // reader's yesterday.
        const QDateTime when =
            QDateTime::fromMSecsSinceEpoch(e.timestampMs).toLocalTime();
        const QDate day = when.date();
        const QDate today = QDate::currentDate();
        if (day == today)
            return tr("Today");
        if (day == today.addDays(-1))
            return tr("Yesterday");
        if (day > today.addDays(-7))
            return tr("This week");
        if (day.year() == today.year() && day.month() == today.month())
            return tr("This month");
        // Month order and names come from the viewer's locale.
        return QLocale().toString(day, QStringLiteral("MMMM yyyy"));
    }
    default: return {};
    }
}

void MediaHistoryModel::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    clearAll();
    Q_EMIT roomIdChanged();
    // No auto-load: walking history for a tab nobody opened is an unrequested
    // request. QML calls loadMore() when the browser becomes visible.
}

void MediaHistoryModel::setCategory(const QString &category)
{
    if (m_category == category)
        return;
    m_category = category;
    rebuild();
    Q_EMIT filtersChanged();
}

void MediaHistoryModel::setSenderFilter(const QString &sender)
{
    if (m_sender == sender)
        return;
    m_sender = sender;
    rebuild();
    Q_EMIT filtersChanged();
}

void MediaHistoryModel::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    rebuild();
    Q_EMIT filtersChanged();
}

void MediaHistoryModel::setFromDate(const QDateTime &from)
{
    if (m_from == from)
        return;
    m_from = from;
    rebuild();
    Q_EMIT filtersChanged();
}

void MediaHistoryModel::setToDate(const QDateTime &to)
{
    if (m_to == to)
        return;
    m_to = to;
    rebuild();
    Q_EMIT filtersChanged();
}

void MediaHistoryModel::loadMore()
{
    // One request at a time and none once complete; a grid calls this on every
    // scroll.
    if (!available() || m_roomId.isEmpty() || m_pendingOp != 0 || m_complete)
        return;
    m_lastError.clear();
    m_pendingOp = m_client->requestMediaHistoryPage(m_roomId, kPageSize, false);
    if (m_pendingOp == 0)
        return;   // the backend refused; nothing is in flight
    Q_EMIT stateChanged();
}

void MediaHistoryModel::reload()
{
    if (!available() || m_roomId.isEmpty())
        return;
    clearAll();
    m_pendingOp = m_client->requestMediaHistoryPage(m_roomId, kPageSize, true);
    Q_EMIT stateChanged();
}

QVariantMap MediaHistoryModel::entryAt(int row) const
{
    if (row < 0 || row >= m_shown.size())
        return {};
    const Entry &e = m_all.at(m_shown.at(row));
    return QVariantMap{
        { QStringLiteral("eventId"), e.eventId },
        { QStringLiteral("sender"), e.sender },
        { QStringLiteral("timestampMs"), e.timestampMs },
        { QStringLiteral("kind"), e.kind },
        { QStringLiteral("body"), e.body },
        { QStringLiteral("filename"), e.filename },
        { QStringLiteral("mimetype"), e.mimetype },
        { QStringLiteral("size"), e.size },
        { QStringLiteral("mxc"), e.mxc },
        { QStringLiteral("thumbnailMxc"), e.thumbnailMxc },
        { QStringLiteral("mediaKey"), e.mediaKey },
        { QStringLiteral("encrypted"), e.encrypted },
        { QStringLiteral("url"), e.url },
        { QStringLiteral("host"), e.host },
    };
}

QVariantList MediaHistoryModel::imageEntries() const
{
    QVariantList out;
    for (int row = 0; row < m_shown.size(); ++row) {
        const Entry &e = m_all.at(m_shown.at(row));
        if (e.kind != QLatin1String("image") || e.mediaKey.isEmpty())
            continue;
        QVariantMap entry;
        // `row` is the shown row this entry came from, as TimelineModel's
        // entries carry their row.
        entry.insert(QStringLiteral("row"), row);
        entry.insert(QStringLiteral("mediaKey"), e.mediaKey);
        entry.insert(QStringLiteral("filename"),
                     e.filename.isEmpty() ? e.body : e.filename);
        entry.insert(QStringLiteral("sender"), e.sender);
        entry.insert(QStringLiteral("timestamp"),
                     QDateTime::fromMSecsSinceEpoch(e.timestampMs));
        entry.insert(QStringLiteral("mime"), e.mimetype);
        // No HTTP fallback: this walk exists only on the Rust backend, where
        // bytes go through the authenticated media bridge. Never synthesise an
        // unauthenticated URL here.
        entry.insert(QStringLiteral("httpUrl"), QUrl{});
        entry.insert(QStringLiteral("isImage"), true);
        entry.insert(QStringLiteral("isVideo"), false);
        entry.insert(QStringLiteral("isVisual"), true);
        entry.insert(QStringLiteral("thumbAvailable"), !e.thumbnailMxc.isEmpty());
        entry.insert(QStringLiteral("size"), e.size);
        out.append(entry);
    }
    return out;
}

int MediaHistoryModel::imageIndexForRow(int row) const
{
    if (row < 0 || row >= m_shown.size())
        return -1;
    int index = 0;
    for (int r = 0; r < m_shown.size(); ++r) {
        const Entry &e = m_all.at(m_shown.at(r));
        if (e.kind != QLatin1String("image") || e.mediaKey.isEmpty()) {
            if (r == row)
                return -1; // the row exists but is not one of the images
            continue;
        }
        if (r == row)
            return index;
        ++index;
    }
    return -1;
}

QStringList MediaHistoryModel::knownSenders() const
{
    QStringList out;
    QSet<QString> seen;
    for (const Entry &e : m_all) {
        if (e.sender.isEmpty() || seen.contains(e.sender))
            continue;
        seen.insert(e.sender);
        out.append(e.sender);
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

void MediaHistoryModel::onPage(quint64 opId, const QString &roomId,
                               const QVariantList &entries, qint64 scanned,
                               qint64 scannedTotal, qint64 undecryptable,
                               bool complete, bool encryptedRoom)
{
    Q_UNUSED(scanned);
    // Drop pages for another room or a superseded walk, so one room's media
    // never appears under another's name.
    if (opId != m_pendingOp || roomId != m_roomId)
        return;
    m_pendingOp = 0;
    m_scannedTotal = scannedTotal;
    m_undecryptable = undecryptable;
    m_complete = complete;
    m_encryptedRoom = encryptedRoom;

    QVector<Entry> added;
    added.reserve(entries.size());
    for (const QVariant &value : entries) {
        const QVariantMap row = value.toMap();
        Entry e;
        e.eventId = row.value(QStringLiteral("eventId")).toString();
        e.kind = row.value(QStringLiteral("kind")).toString();
        e.url = row.value(QStringLiteral("url")).toString();
        // A link event contributes one row per URL, so identity is the (event,
        // URL) pair, joined with a unit separator that cannot occur in an event
        // id.
        const QString identity = e.eventId + QChar(0x1F) + e.url;
        if (e.eventId.isEmpty() || m_seen.contains(identity))
            continue;
        m_seen.insert(identity);
        e.sender = row.value(QStringLiteral("sender")).toString();
        e.timestampMs = row.value(QStringLiteral("timestampMs")).toLongLong();
        e.body = row.value(QStringLiteral("body")).toString();
        e.filename = row.value(QStringLiteral("filename")).toString();
        e.mimetype = row.value(QStringLiteral("mimetype")).toString();
        e.size = row.value(QStringLiteral("size")).toLongLong();
        e.width = row.value(QStringLiteral("width")).toInt();
        e.height = row.value(QStringLiteral("height")).toInt();
        e.durationMs = row.value(QStringLiteral("durationMs")).toLongLong();
        e.mxc = row.value(QStringLiteral("mxc")).toString();
        e.thumbnailMxc = row.value(QStringLiteral("thumbnailMxc")).toString();
        e.mediaKey = row.value(QStringLiteral("mediaKey")).toString();
        e.encrypted = row.value(QStringLiteral("encrypted")).toBool();
        e.host = row.value(QStringLiteral("host")).toString();
        added.append(e);
    }

    if (!added.isEmpty()) {
        m_all.append(added);
        // Pages already arrive in order; the sort also covers a server
        // answering out of order.
        std::stable_sort(m_all.begin(), m_all.end(),
                         [](const Entry &a, const Entry &b) {
                             return newerFirst(a.timestampMs, b.timestampMs);
                         });
    }
    rebuild();
    Q_EMIT stateChanged();
    Q_EMIT countsChanged();
}

void MediaHistoryModel::onFailed(quint64 opId, const QString &roomId,
                                 const QString &message)
{
    if (opId != m_pendingOp || roomId != m_roomId)
        return;
    m_pendingOp = 0;
    // Not `complete`: after an error the rest of history is unknown, and the
    // view offers a retry.
    m_lastError = message;
    Q_EMIT stateChanged();
}

bool MediaHistoryModel::matches(const Entry &e) const
{
    if (!m_category.isEmpty()) {
        if (m_category == QLatin1String("media")) {
            if (e.kind != QLatin1String("image")
                && e.kind != QLatin1String("video"))
                return false;
        } else if (m_category == QLatin1String("audio")) {
            // Voice messages are m.audio and belong in the Audio tab.
            if (e.kind != QLatin1String("audio")
                && e.kind != QLatin1String("voice"))
                return false;
        } else if (e.kind != m_category) {
            return false;
        }
    }
    if (!m_sender.isEmpty() && e.sender != m_sender)
        return false;
    if (m_from.isValid() && e.timestampMs < m_from.toMSecsSinceEpoch())
        return false;
    if (m_to.isValid() && e.timestampMs > m_to.toMSecsSinceEpoch())
        return false;
    if (!m_query.isEmpty()) {
        const Qt::CaseSensitivity ci = Qt::CaseInsensitive;
        if (!e.filename.contains(m_query, ci) && !e.body.contains(m_query, ci)
            && !e.sender.contains(m_query, ci) && !e.url.contains(m_query, ci)
            && !e.host.contains(m_query, ci)
            && !e.mimetype.contains(m_query, ci))
            return false;
    }
    return true;
}

void MediaHistoryModel::rebuild()
{
    beginResetModel();
    m_shown.clear();
    m_shown.reserve(m_all.size());
    for (int i = 0; i < m_all.size(); ++i) {
        if (matches(m_all.at(i)))
            m_shown.append(i);
    }
    endResetModel();
    Q_EMIT countsChanged();
}

void MediaHistoryModel::clearAll()
{
    beginResetModel();
    m_all.clear();
    m_shown.clear();
    m_seen.clear();
    endResetModel();
    m_pendingOp = 0;
    m_complete = false;
    m_encryptedRoom = false;
    m_scannedTotal = 0;
    m_undecryptable = 0;
    m_lastError.clear();
    Q_EMIT stateChanged();
    Q_EMIT countsChanged();
}
