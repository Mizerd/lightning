#include "models/TimelineModel.h"

#include "matrix/MatrixClient.h"

#include <QUrl>
#include <QVariantMap>

TimelineModel::TimelineModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void TimelineModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::eventAppended,
                this, &TimelineModel::onEventAppended);
        connect(m_client, &MatrixClient::eventReplaced,
                this, &TimelineModel::onEventReplaced);
        connect(m_client, &MatrixClient::eventStatusChanged,
                this, &TimelineModel::onEventStatusChanged);
        connect(m_client, &MatrixClient::eventEdited,
                this, &TimelineModel::onEventEdited);
        connect(m_client, &MatrixClient::eventRedacted,
                this, &TimelineModel::onEventRedacted);
        connect(m_client, &MatrixClient::reactionsChanged,
                this, &TimelineModel::onReactionsChanged);
        connect(m_client, &MatrixClient::eventsPrepended,
                this, &TimelineModel::onEventsPrepended);
        connect(m_client, &MatrixClient::timelineReset,
                this, &TimelineModel::onTimelineReset);
        connect(m_client, &MatrixClient::loggedOut,
                this, &TimelineModel::onLoggedOut);
        connect(m_client, &MatrixClient::typingChanged,
                this, &TimelineModel::onTypingChanged);
        connect(m_client, &MatrixClient::membersChanged,
                this, &TimelineModel::onMembersChanged);
        connect(m_client, &MatrixClient::paginationStateChanged,
                this, &TimelineModel::onPaginationStateChanged);
        m_selfUserId = m_client->currentUserId();
        connect(m_client, &MatrixClient::loginSucceeded, this,
                [this](const QString &userId) { m_selfUserId = userId; });
    }
    reload();
    refreshTypingText();
    Q_EMIT paginationChanged();
}

void TimelineModel::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    Q_EMIT roomIdChanged();
    reload();
    refreshTypingText();
    Q_EMIT paginationChanged();
}

int TimelineModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_events.size());
}

QUrl TimelineModel::mediaHttp(const QString &mxc) const
{
    if (!m_client || mxc.isEmpty()) return {};
    return m_client->mediaDownloadUrl(mxc);
}

QUrl TimelineModel::mediaThumbHttp(const QString &mxc, int w, int h) const
{
    if (!m_client || mxc.isEmpty()) return {};
    return m_client->mediaThumbnailUrl(mxc, w, h, false);
}

QVariantList TimelineModel::reactionsVariant(const TimelineEvent &e) const
{
    QVariantList out;
    out.reserve(e.reactions.size());
    for (const auto &r : e.reactions) {
        QVariantMap m;
        m.insert(QStringLiteral("key"),   r.key);
        m.insert(QStringLiteral("count"), r.count);
        m.insert(QStringLiteral("byMe"),  r.byMe);
        out.append(m);
    }
    return out;
}

QVariant TimelineModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_events.size())
        return {};
    const auto &e = m_events.at(index.row());
    switch (role) {
    case EventIdRole:            return e.eventId;
    case SenderRole:             return e.sender;
    case SenderDisplayNameRole: {
        if (!e.senderDisplayName.isEmpty()) return e.senderDisplayName;
        if (m_client) return m_client->displayNameFor(e.roomId, e.sender);
        return e.sender;
    }
    case BodyRole: {
        if (e.redacted) return QStringLiteral("[message deleted]");
        return e.body;
    }
    case TimestampRole:          return e.timestamp;
    case TypeRole:               return static_cast<int>(e.type);
    case StatusRole:             return static_cast<int>(e.status);
    case IsOwnRole:              return e.sender == m_selfUserId;
    case EditedRole:             return e.edited;
    case RedactedRole:           return e.redacted;
    case ReplyToEventIdRole:     return e.replyToEventId;
    case ReplyToSenderRole:      return e.replyToSender;
    case ReplyToPreviewRole:     return e.replyToPreview;
    case MediaMxcUrlRole:        return e.mediaMxcUrl;
    case MediaHttpUrlRole:       return mediaHttp(e.mediaMxcUrl);
    case MediaThumbnailHttpUrlRole: {
        const QString mxc = e.mediaThumbnailMxcUrl.isEmpty()
            ? e.mediaMxcUrl : e.mediaThumbnailMxcUrl;
        return mediaThumbHttp(mxc, 800, 600);
    }
    case MediaMimetypeRole:      return e.mediaMimetype;
    case MediaFilenameRole:      return e.mediaFilename;
    case MediaSizeRole:          return static_cast<qint64>(e.mediaSize);
    case MediaWidthRole:         return e.mediaWidth;
    case MediaHeightRole:        return e.mediaHeight;
    case IsImageRole:            return e.type == TimelineEvent::Image;
    case IsFileRole:             return e.type == TimelineEvent::File;
    case ReactionsRole:          return reactionsVariant(e);
    default:                     return {};
    }
}

QHash<int, QByteArray> TimelineModel::roleNames() const
{
    return {
        { EventIdRole,             "eventId" },
        { SenderRole,              "sender" },
        { SenderDisplayNameRole,   "senderDisplayName" },
        { BodyRole,                "body" },
        { TimestampRole,           "timestamp" },
        { TypeRole,                "eventType" },
        { StatusRole,              "status" },
        { IsOwnRole,               "isOwn" },
        { EditedRole,              "edited" },
        { RedactedRole,            "redacted" },
        { ReplyToEventIdRole,      "replyToEventId" },
        { ReplyToSenderRole,       "replyToSender" },
        { ReplyToPreviewRole,      "replyToPreview" },
        { MediaMxcUrlRole,         "mediaMxc" },
        { MediaHttpUrlRole,        "mediaUrl" },
        { MediaThumbnailHttpUrlRole,"mediaThumbUrl" },
        { MediaMimetypeRole,       "mediaMimetype" },
        { MediaFilenameRole,       "mediaFilename" },
        { MediaSizeRole,           "mediaSize" },
        { MediaWidthRole,          "mediaWidth" },
        { MediaHeightRole,         "mediaHeight" },
        { IsImageRole,             "isImage" },
        { IsFileRole,              "isFile" },
        { ReactionsRole,           "reactions" },
    };
}

int TimelineModel::rowForEventId(const QString &eventId) const
{
    for (int i = 0; i < m_events.size(); ++i) {
        if (m_events.at(i).eventId == eventId)
            return i;
    }
    return -1;
}

void TimelineModel::onEventAppended(const QString &roomId, const TimelineEvent &event)
{
    if (roomId != m_roomId)
        return;
    const int row = m_events.size();
    beginInsertRows({}, row, row);
    m_events.append(event);
    endInsertRows();
    Q_EMIT countChanged();
}

void TimelineModel::onEventReplaced(const QString &roomId,
                                     const QString &oldEventId,
                                     const TimelineEvent &newEvent)
{
    if (roomId != m_roomId)
        return;
    const int row = rowForEventId(oldEventId);
    if (row < 0)
        return;
    m_events[row] = newEvent;
    const auto idx = index(row);
    Q_EMIT dataChanged(idx, idx);
}

void TimelineModel::onEventStatusChanged(const QString &roomId,
                                          const QString &eventId,
                                          TimelineEvent::Status status)
{
    if (roomId != m_roomId)
        return;
    const int row = rowForEventId(eventId);
    if (row < 0)
        return;
    m_events[row].status = status;
    const auto idx = index(row);
    Q_EMIT dataChanged(idx, idx, { StatusRole });
}

void TimelineModel::onEventEdited(const QString &roomId, const QString &eventId)
{
    if (roomId != m_roomId) return;
    if (!m_client) return;
    // Pull fresh event data from client cache.
    const auto latest = m_client->timeline(m_roomId);
    for (const auto &e : latest) {
        if (e.eventId == eventId) {
            const int row = rowForEventId(eventId);
            if (row < 0) return;
            m_events[row] = e;
            const auto idx = index(row);
            Q_EMIT dataChanged(idx, idx, { BodyRole, EditedRole });
            return;
        }
    }
}

void TimelineModel::onEventRedacted(const QString &roomId, const QString &eventId)
{
    if (roomId != m_roomId) return;
    const int row = rowForEventId(eventId);
    if (row < 0) return;
    m_events[row].redacted = true;
    m_events[row].body.clear();
    const auto idx = index(row);
    Q_EMIT dataChanged(idx, idx, { BodyRole, RedactedRole, ReactionsRole });
}

void TimelineModel::onReactionsChanged(const QString &roomId, const QString &eventId)
{
    if (roomId != m_roomId) return;
    if (!m_client) return;
    const int row = rowForEventId(eventId);
    if (row < 0) return;
    const auto latest = m_client->timeline(m_roomId);
    for (const auto &e : latest) {
        if (e.eventId == eventId) {
            m_events[row].reactions = e.reactions;
            const auto idx = index(row);
            Q_EMIT dataChanged(idx, idx, { ReactionsRole });
            return;
        }
    }
}

void TimelineModel::onEventsPrepended(const QString &roomId,
                                       const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId) return;
    if (events.isEmpty()) return;
    beginInsertRows({}, 0, events.size() - 1);
    for (int i = events.size() - 1; i >= 0; --i)
        m_events.prepend(events.at(i));
    endInsertRows();
    Q_EMIT countChanged();
}

void TimelineModel::onTimelineReset(const QString &roomId)
{
    if (roomId != m_roomId)
        return;
    reload();
}

void TimelineModel::onLoggedOut()
{
    beginResetModel();
    m_events.clear();
    m_roomId.clear();
    endResetModel();
    Q_EMIT roomIdChanged();
    Q_EMIT countChanged();
    m_typingText.clear();
    Q_EMIT typingTextChanged();
    Q_EMIT paginationChanged();
}

void TimelineModel::onTypingChanged(const QString &roomId)
{
    if (roomId != m_roomId) return;
    refreshTypingText();
}

void TimelineModel::onMembersChanged(const QString &roomId)
{
    if (roomId != m_roomId) return;
    // Refresh display-name column for every row (cheap: emit dataChanged once).
    if (m_events.isEmpty()) return;
    Q_EMIT dataChanged(index(0), index(m_events.size() - 1),
                       { SenderDisplayNameRole });
    refreshTypingText();
}

void TimelineModel::onPaginationStateChanged(const QString &roomId)
{
    if (roomId != m_roomId) return;
    Q_EMIT paginationChanged();
}

void TimelineModel::refreshTypingText()
{
    QString next;
    if (m_client && !m_roomId.isEmpty()) {
        const auto users = m_client->typingUsersFor(m_roomId);
        QStringList names;
        for (const auto &u : users) {
            if (u == m_selfUserId) continue;
            names.append(m_client->displayNameFor(m_roomId, u));
            if (names.size() >= 3) break;
        }
        if (users.size() > names.size())
            names.append(tr("others"));
        if (names.size() == 1)
            next = tr("%1 is typing…").arg(names.first());
        else if (names.size() > 1)
            next = tr("%1 are typing…").arg(names.join(QStringLiteral(", ")));
    }
    if (next != m_typingText) {
        m_typingText = next;
        Q_EMIT typingTextChanged();
    }
}

void TimelineModel::requestOlder()
{
    if (!m_client || m_roomId.isEmpty()) return;
    m_client->loadOlderMessages(m_roomId);
}

void TimelineModel::markVisibleAsRead(int firstVisibleRow, int lastVisibleRow)
{
    Q_UNUSED(firstVisibleRow);
    if (!m_client || m_roomId.isEmpty()) return;
    if (lastVisibleRow < 0 || lastVisibleRow >= m_events.size()) return;
    const QString eventId = m_events.at(lastVisibleRow).eventId;
    if (eventId.startsWith(QLatin1String("local:"))) return; // don't ack unsent
    m_client->sendReadReceipt(m_roomId, eventId);
}

bool TimelineModel::canPaginate() const
{
    if (!m_client || m_roomId.isEmpty()) return false;
    return m_client->canPaginate(m_roomId);
}

bool TimelineModel::paginating() const
{
    if (!m_client || m_roomId.isEmpty()) return false;
    return m_client->paginating(m_roomId);
}

void TimelineModel::reload()
{
    beginResetModel();
    m_events = (m_client && !m_roomId.isEmpty())
                   ? m_client->timeline(m_roomId)
                   : QList<TimelineEvent>{};
    endResetModel();
    Q_EMIT countChanged();
}
