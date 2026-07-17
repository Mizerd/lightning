#include "models/TimelineModel.h"

#include "matrix/MatrixClient.h"

#include <QRegularExpression>
#include <QUrl>
#include <QVariantMap>

namespace {
// Element/Discord-style visual grouping window. Five minutes is short enough
// to keep conversations scannable while suppressing repetitive identity.
constexpr qint64 kSenderGroupThresholdSeconds = 5 * 60;
}

TimelineModel::TimelineModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

QString TimelineModel::senderDisplayName(const TimelineEvent &event) const
{
    if (!event.senderDisplayName.isEmpty())
        return event.senderDisplayName;
    if (m_client) {
        const QString display = m_client->displayNameFor(event.roomId, event.sender);
        if (!display.isEmpty())
            return display;
    }
    return event.sender;
}

QString TimelineModel::senderInitials(const TimelineEvent &event) const
{
    QString name = senderDisplayName(event).trimmed();
    if (name.startsWith(QLatin1Char('@')))
        name = name.mid(1).section(QLatin1Char(':'), 0, 0);
    const QStringList words = name.split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return QStringLiteral("?");
    QString initials = words.first().left(1);
    if (words.size() > 1)
        initials += words.last().left(1);
    return initials.toUpper();
}

bool TimelineModel::isVisualMessage(const TimelineEvent &event) const
{
    return !event.isVirtual() && event.type != TimelineEvent::StateChange;
}

int TimelineModel::previousMessageRowForGrouping(int row) const
{
    for (int probe = row - 1; probe >= 0; --probe) {
        const auto &candidate = m_events.at(probe);
        if (candidate.type == TimelineEvent::ReadMarker)
            continue;
        if (!isVisualMessage(candidate))
            return -1;
        return probe;
    }
    return -1;
}

int TimelineModel::nextMessageRowForGrouping(int row) const
{
    for (int probe = row + 1; probe < m_events.size(); ++probe) {
        const auto &candidate = m_events.at(probe);
        if (candidate.type == TimelineEvent::ReadMarker)
            continue;
        if (!isVisualMessage(candidate))
            return -1;
        return probe;
    }
    return -1;
}

bool TimelineModel::continuesSenderGroup(int row) const
{
    if (row < 0 || row >= m_events.size())
        return false;
    const auto &event = m_events.at(row);
    if (!isVisualMessage(event) || event.redacted)
        return false;
    const int previousRow = previousMessageRowForGrouping(row);
    if (previousRow < 0)
        return false;
    const auto &previous = m_events.at(previousRow);
    if (previous.redacted || previous.sender != event.sender
        || previous.roomId != event.roomId
        || !previous.timestamp.isValid() || !event.timestamp.isValid()
        || previous.timestamp.date() != event.timestamp.date())
        return false;
    const qint64 gap = previous.timestamp.secsTo(event.timestamp);
    return gap >= 0 && gap < kSenderGroupThresholdSeconds;
}

void TimelineModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_selfUserId = m_client ? m_client->currentUserId() : QString{};
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
        connect(m_client, &MatrixClient::eventInsertedAt,
                this, &TimelineModel::onEventInsertedAt);
        connect(m_client, &MatrixClient::eventChangedAt,
                this, &TimelineModel::onEventChangedAt);
        connect(m_client, &MatrixClient::eventRemovedAt,
                this, &TimelineModel::onEventRemovedAt);
        connect(m_client, &MatrixClient::eventsTruncatedTo,
                this, &TimelineModel::onEventsTruncatedTo);
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

// Virtual rows (date dividers, read markers, the timeline-start marker) are
// synthetic SDK bookkeeping items, not visible messages — matrix-sdk-ui
// freely interleaves them between real events (a date divider at every day
// boundary, a read marker wherever the user last read up to). They must not
// split a run of state-change events into separate activity groups; only an
// actual visible message/media event (or the start/end of the timeline)
// ends a group.
int TimelineModel::stateGroupLeaderRow(int row) const
{
    if (row < 0 || row >= m_events.size()
        || m_events.at(row).type != TimelineEvent::StateChange)
        return -1;

    int leader = row;
    int probe = row - 1;
    while (probe >= 0) {
        const auto &e = m_events.at(probe);
        if (e.type == TimelineEvent::StateChange) {
            leader = probe;
            --probe;
            continue;
        }
        if (e.isVirtual()) {
            --probe;
            continue;
        }
        break; // a visible message/media event ends the group here.
    }
    return leader;
}

QVariantList TimelineModel::stateGroupEntriesFrom(int leaderRow) const
{
    QVariantList entries;
    for (int i = leaderRow; i < m_events.size();) {
        const auto &e = m_events.at(i);
        if (e.type == TimelineEvent::StateChange) {
            QVariantMap entry;
            entry.insert(QStringLiteral("stableEventId"),
                         e.itemId.isEmpty() ? e.eventId : e.itemId);
            entry.insert(QStringLiteral("eventId"), e.eventId);
            entry.insert(QStringLiteral("eventKind"), e.stateKind);
            entry.insert(QStringLiteral("actorUserId"), e.sender);
            QString actor = e.senderDisplayName;
            if (actor.isEmpty() && m_client)
                actor = m_client->displayNameFor(e.roomId, e.sender);
            if (actor.isEmpty())
                actor = e.sender;
            entry.insert(QStringLiteral("actorDisplayName"), actor);
            entry.insert(QStringLiteral("affectedMemberDisplayName"), e.stateTarget);
            entry.insert(QStringLiteral("description"), e.body);
            entry.insert(QStringLiteral("timestamp"), e.timestamp);
            entries.append(entry);
            ++i;
            continue;
        }
        if (e.isVirtual()) {
            ++i;
            continue;
        }
        break;
    }
    return entries;
}

void TimelineModel::emitPresentationGroupingChanged()
{
    if (m_events.isEmpty())
        return;
    Q_EMIT dataChanged(index(0), index(m_events.size() - 1),
                       { StateGroupIdRole, StateGroupLeaderRole,
                         StateGroupEntriesRole, SameSenderAsPreviousRole,
                         BeginsSenderGroupRole, ContinuesSenderGroupRole,
                         EndsSenderGroupRole, ShowSenderIdentityRole });
}

QVariant TimelineModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_events.size())
        return {};
    const auto &e = m_events.at(index.row());
    switch (role) {
    case EventIdRole:            return e.eventId;
    case SenderRole:             return e.sender;
    case SenderDisplayNameRole: return senderDisplayName(e);
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
    case ThreadRootIdRole:       return e.threadRootId;
    case IsThreadRootRole: {
        // v0.6.0: the SDK's bundled thread summary is authoritative when
        // present; otherwise scan the loaded timeline (mock/HTTP backends).
        if (e.isThreadRoot)
            return true;
        for (const auto &other : m_events) {
            if (other.threadRootId == e.eventId) return true;
        }
        return false;
    }
    case ThreadReplyCountRole: {
        if (e.threadReplyCount >= 0)
            return e.threadReplyCount;   // SDK summary (server aggregation)
        int c = 0;
        for (const auto &other : m_events) {
            if (other.threadRootId == e.eventId) ++c;
        }
        return c;
    }
    case ThreadLatestPreviewRole:   return e.threadLatestPreview;
    case ThreadLatestSenderRole:    return e.threadLatestSender;
    case ThreadLatestTimestampRole: return e.threadLatestTimestamp;
    case ThreadUnreadRole:          return e.threadUnread;
    case MentionsMeRole:            return e.mentionsMe;
    case MentionsRoomRole:          return e.mentionsRoom;
    case IsEncryptedRole:        return e.isEncrypted;
    case IsDecryptedRole:        return e.isDecrypted;
    case UndecryptableRole:      return e.undecryptable;
    case ErrorKindRole:          return e.errorKind;
    case ItemIdRole:             return e.itemId;
    case IsLocalEchoRole:        return e.isLocalEcho;
    case SendErrorRole:          return e.sendErrorCategory;
    case IsVirtualRole:          return e.isVirtual();
    case MediaKeyRole:           return e.mediaKey;
    case MediaSourceAvailableRole: return e.mediaSourceAvailable;
    case MediaThumbAvailableRole:  return e.mediaThumbAvailable;
    case SenderNameAmbiguousRole:  return e.senderNameAmbiguous;
    case SameSenderAsPreviousRole:
    case ContinuesSenderGroupRole: return continuesSenderGroup(index.row());
    case BeginsSenderGroupRole:
    case ShowSenderIdentityRole:
        return isVisualMessage(e) && !continuesSenderGroup(index.row());
    case EndsSenderGroupRole: {
        if (!isVisualMessage(e))
            return false;
        const int next = nextMessageRowForGrouping(index.row());
        return next < 0 || !continuesSenderGroup(next);
    }
    case SenderAvatarMxcRole: {
        if (!e.senderAvatarUrl.isEmpty())
            return e.senderAvatarUrl;
        return m_client ? m_client->avatarMxcFor(e.roomId, e.sender) : QString{};
    }
    case SenderInitialsRole: return senderInitials(e);
    case StableEventIdRole: return e.itemId.isEmpty() ? e.eventId : e.itemId;
    case IsStateActivityRole: return e.type == TimelineEvent::StateChange;
    // Typed membership/profile/room-state events are routine annotations.
    // StateChange rows without a kind include call/RTC notifications and
    // remain visible because they are not proven routine activity.
    case IsRoutineActivityRole:
        return e.type == TimelineEvent::StateChange && !e.stateKind.isEmpty();
    case StateKindRole: return e.stateKind;
    case StateGroupIdRole: {
        const int leader = stateGroupLeaderRow(index.row());
        if (leader < 0) return QString{};
        const auto &first = m_events.at(leader);
        return first.itemId.isEmpty() ? first.eventId : first.itemId;
    }
    case StateGroupLeaderRole: {
        const int leader = stateGroupLeaderRow(index.row());
        return leader == index.row();
    }
    case StateGroupEntriesRole: {
        const int leader = stateGroupLeaderRow(index.row());
        if (leader != index.row()) return QVariantList{};
        return stateGroupEntriesFrom(leader);
    }
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
        { ThreadRootIdRole,        "threadRootId" },
        { IsThreadRootRole,        "isThreadRoot" },
        { ThreadReplyCountRole,    "threadReplyCount" },
        { ThreadLatestPreviewRole,   "threadLatestPreview" },
        { ThreadLatestSenderRole,    "threadLatestSender" },
        { ThreadLatestTimestampRole, "threadLatestTimestamp" },
        { ThreadUnreadRole,          "threadUnread" },
        { MentionsMeRole,            "mentionsMe" },
        { MentionsRoomRole,          "mentionsRoom" },
        { IsEncryptedRole,         "isEncrypted" },
        { IsDecryptedRole,         "isDecrypted" },
        { UndecryptableRole,       "undecryptable" },
        { ErrorKindRole,           "errorKind" },
        { ItemIdRole,              "itemId" },
        { IsLocalEchoRole,         "isLocalEcho" },
        { SendErrorRole,           "sendErrorCategory" },
        { IsVirtualRole,           "isVirtual" },
        { MediaKeyRole,            "mediaKey" },
        { MediaSourceAvailableRole, "mediaSourceAvailable" },
        { MediaThumbAvailableRole, "mediaThumbAvailable" },
        { SenderNameAmbiguousRole, "senderNameAmbiguous" },
        { SameSenderAsPreviousRole, "sameSenderAsPrevious" },
        { IsStateActivityRole,      "isStateActivity" },
        { IsRoutineActivityRole,    "isRoutineActivity" },
        { StateKindRole,            "stateKind" },
        { StateGroupIdRole,         "stateGroupId" },
        { StateGroupLeaderRole,     "stateGroupLeader" },
        { StateGroupEntriesRole,    "stateGroupEntries" },
        { SenderAvatarMxcRole,      "senderAvatarMxc" },
        { SenderInitialsRole,       "senderInitials" },
        { BeginsSenderGroupRole,    "beginsSenderGroup" },
        { ContinuesSenderGroupRole, "continuesSenderGroup" },
        { EndsSenderGroupRole,      "endsSenderGroup" },
        { ShowSenderIdentityRole,   "showSenderIdentity" },
        { StableEventIdRole,        "stableEventId" },
    };
}

QVariantList TimelineModel::imageEntries() const
{
    QVariantList out;
    const QVariantList all = mediaEntries();
    for (const QVariant &value : all) {
        if (value.toMap().value(QStringLiteral("isImage")).toBool())
            out.append(value);
    }
    return out;
}

QVariantList TimelineModel::mediaEntries() const
{
    QVariantList out;
    for (int row = 0; row < m_events.size(); ++row) {
        const TimelineEvent &e = m_events.at(row);
        if ((e.type != TimelineEvent::Image && e.type != TimelineEvent::File)
            || e.redacted)
            continue;
        // Usable when the media bridge can fetch it (Rust) or an HTTP
        // download URL exists (HTTP backend).
        const QUrl httpUrl = mediaHttp(e.mediaMxcUrl);
        if (!e.mediaSourceAvailable && httpUrl.isEmpty())
            continue;
        QVariantMap entry;
        entry.insert(QStringLiteral("row"), row);
        entry.insert(QStringLiteral("mediaKey"), e.mediaKey);
        entry.insert(QStringLiteral("filename"), e.mediaFilename);
        entry.insert(QStringLiteral("sender"),
                     e.senderDisplayName.isEmpty() ? e.sender
                                                   : e.senderDisplayName);
        entry.insert(QStringLiteral("timestamp"), e.timestamp);
        entry.insert(QStringLiteral("mime"), e.mediaMimetype);
        entry.insert(QStringLiteral("httpUrl"), httpUrl);
        entry.insert(QStringLiteral("isImage"), e.type == TimelineEvent::Image);
        entry.insert(QStringLiteral("size"), static_cast<qint64>(e.mediaSize));
        out.append(entry);
    }
    return out;
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
    emitPresentationGroupingChanged();
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
    Q_EMIT dataChanged(index(0), index(m_events.size() - 1));
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
    emitPresentationGroupingChanged();
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
    emitPresentationGroupingChanged();
    // v0.5.11: scroll-anchor hook. A backward-pagination prepend shifts
    // every existing row down by `count`; QML re-anchors on the stable id
    // it captured before requesting the batch.
    Q_EMIT olderPrepended(static_cast<int>(events.size()));
}

void TimelineModel::onTimelineReset(const QString &roomId)
{
    if (roomId != m_roomId)
        return;
    reload();
}

void TimelineModel::onEventInsertedAt(const QString &roomId, int index,
                                      const TimelineEvent &event)
{
    if (roomId != m_roomId)
        return;
    if (index < 0 || index > m_events.size()) {
        // Never apply a corrupt index — self-heal from the backend copy.
        reload();
        return;
    }
    beginInsertRows({}, index, index);
    m_events.insert(index, event);
    endInsertRows();
    Q_EMIT countChanged();
    emitPresentationGroupingChanged();
}

void TimelineModel::onEventChangedAt(const QString &roomId, int index,
                                     const TimelineEvent &event)
{
    if (roomId != m_roomId)
        return;
    if (index < 0 || index >= m_events.size()) {
        reload();
        return;
    }
    m_events[index] = event;
    Q_EMIT dataChanged(this->index(0), this->index(m_events.size() - 1));
}

void TimelineModel::onEventRemovedAt(const QString &roomId, int index)
{
    if (roomId != m_roomId)
        return;
    if (index < 0 || index >= m_events.size()) {
        reload();
        return;
    }
    beginRemoveRows({}, index, index);
    m_events.removeAt(index);
    endRemoveRows();
    Q_EMIT countChanged();
    emitPresentationGroupingChanged();
}

void TimelineModel::onEventsTruncatedTo(const QString &roomId, int length)
{
    if (roomId != m_roomId)
        return;
    if (length < 0 || length > m_events.size()) {
        reload();
        return;
    }
    if (length == m_events.size())
        return;
    beginRemoveRows({}, length, m_events.size() - 1);
    while (m_events.size() > length)
        m_events.removeLast();
    endRemoveRows();
    Q_EMIT countChanged();
    emitPresentationGroupingChanged();
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
    // Refresh SDK/member-derived identity for every row (cheap: one signal).
    if (m_events.isEmpty()) return;
    Q_EMIT dataChanged(index(0), index(m_events.size() - 1),
                       { SenderDisplayNameRole, SenderInitialsRole,
                         SenderAvatarMxcRole });
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
            if (names.size() >= 2) break;
        }
        if (users.size() == 1 && names.size() == 1)
            next = tr("%1 is typing…").arg(names.first());
        else if (users.size() == 2 && names.size() == 2)
            next = tr("%1 and %2 are typing…").arg(names.at(0), names.at(1));
        else if (users.size() >= 3)
            next = tr("%1 people are typing…").arg(users.size());
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
    Q_UNUSED(lastVisibleRow);
    if (!m_client || m_roomId.isEmpty()) return;
    // v0.5.11: the scan is shared with ReadReceiptCoordinator. This direct
    // path remains for explicit user gestures; the automatic policy
    // (focus/visibility/debounce) lives in the coordinator.
    const QString eventId = latestReadableEventId();
    if (!eventId.isEmpty())
        m_client->sendReadReceipt(m_roomId, eventId); // deduped downstream
}

QString TimelineModel::latestReadableEventId(qint64 *timestampMs) const
{
    if (timestampMs)
        *timestampMs = 0;
    // Scan backward for the newest event that carries a real remote event
    // ID. The last row is often a virtual item (SDK read marker, date
    // divider) or a local echo — acking only the literal count-1 row
    // silently failed in those cases, which is why a live incoming message
    // stayed unread until a manual Mark as read (RoomListModel::markRoomRead
    // scans backward the same way).
    for (int i = static_cast<int>(m_events.size()) - 1; i >= 0; --i) {
        const auto &e = m_events.at(i);
        if (e.isVirtual()) continue;                          // date divider / marker
        if (e.eventId.isEmpty()) continue;                    // no remote id yet
        if (e.eventId.startsWith(QLatin1String("local:"))) continue; // unsent echo
        if (e.status == TimelineEvent::Failed) continue;      // failed outgoing
        if (timestampMs && e.timestamp.isValid())
            *timestampMs = e.timestamp.toMSecsSinceEpoch();
        return e.eventId;
    }
    return {};
}

QString TimelineModel::stableIdAt(int row) const
{
    if (row < 0 || row >= m_events.size())
        return {};
    const auto &e = m_events.at(row);
    // The SDK item id survives in-place updates (local echo reconciliation,
    // late decryption); prefer it and fall back to the event id.
    return e.itemId.isEmpty() ? e.eventId : e.itemId;
}

QString TimelineModel::eventIdAt(int row) const
{
    if (row < 0 || row >= m_events.size())
        return {};
    return m_events.at(row).eventId;
}

int TimelineModel::rowForStableId(const QString &stableId) const
{
    if (stableId.isEmpty())
        return -1;
    for (int i = 0; i < m_events.size(); ++i) {
        const auto &e = m_events.at(i);
        if (e.itemId == stableId || e.eventId == stableId)
            return i;
    }
    return -1;
}

const TimelineEvent *TimelineModel::eventForId(const QString &eventId) const
{
    if (eventId.isEmpty())
        return nullptr;
    for (const auto &event : m_events) {
        if (event.eventId == eventId)
            return &event;
    }
    return nullptr;
}

QString TimelineModel::visibleTextForEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->isVirtual() || event->type == TimelineEvent::StateChange
        || event->redacted)
        return {};
    return event->body;
}

QString TimelineModel::messagePermalink(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->roomId.isEmpty() || event->eventId.isEmpty()
        || event->eventId.startsWith(QLatin1String("local:")))
        return {};
    const auto encodeId = [](const QString &id) {
        return QString::fromLatin1(QUrl::toPercentEncoding(
            id, QByteArrayLiteral("!$:@")));
    };
    return QStringLiteral("https://matrix.to/#/%1/%2")
        .arg(encodeId(event->roomId), encodeId(event->eventId));
}

bool TimelineModel::canRedactEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    return event && event->sender == m_selfUserId && !event->redacted
        && !event->isVirtual() && event->type != TimelineEvent::StateChange
        && !event->eventId.startsWith(QLatin1String("local:"));
}

bool TimelineModel::canEditEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    return canRedactEvent(eventId) && event
        && event->type == TimelineEvent::TextMessage
        && event->status == TimelineEvent::Sent;
}

QVariantMap TimelineModel::messageDetails(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->isVirtual() || event->type == TimelineEvent::StateChange)
        return {};

    QString type;
    switch (event->type) {
    case TimelineEvent::TextMessage: type = QStringLiteral("m.room.message (text)"); break;
    case TimelineEvent::Emote:       type = QStringLiteral("m.room.message (emote)"); break;
    case TimelineEvent::Notice:      type = QStringLiteral("m.room.message (notice)"); break;
    case TimelineEvent::Image:       type = QStringLiteral("m.room.message (image)"); break;
    case TimelineEvent::File:        type = QStringLiteral("m.room.message (file)"); break;
    case TimelineEvent::Unknown:     type = QStringLiteral("Unknown message"); break;
    default:                         return {};
    }

    QString delivery;
    if (event->isLocalEcho)
        delivery = QStringLiteral("Local echo");
    else if (event->status == TimelineEvent::Sending)
        delivery = QStringLiteral("Sending");
    else if (event->status == TimelineEvent::Failed)
        delivery = QStringLiteral("Failed");
    else
        delivery = QStringLiteral("Sent");

    QString encryption = QStringLiteral("Not encrypted");
    QString decryption = QStringLiteral("Not applicable");
    if (event->isEncrypted) {
        encryption = QStringLiteral("Encrypted");
        if (event->undecryptable)
            decryption = QStringLiteral("Unable to decrypt");
        else if (event->isDecrypted)
            decryption = QStringLiteral("Decrypted");
        else
            decryption = QStringLiteral("Pending");
    }

    QVariantMap details;
    details.insert(QStringLiteral("senderName"), senderDisplayName(*event));
    details.insert(QStringLiteral("senderId"), event->sender);
    details.insert(QStringLiteral("timestamp"), event->timestamp.toString(Qt::ISODate));
    details.insert(QStringLiteral("roomId"), event->roomId);
    details.insert(QStringLiteral("eventId"), event->eventId);
    details.insert(QStringLiteral("eventType"), type);
    details.insert(QStringLiteral("edited"), event->edited);
    details.insert(QStringLiteral("delivery"), delivery);
    details.insert(QStringLiteral("encryption"), encryption);
    details.insert(QStringLiteral("decryption"), decryption);
    details.insert(QStringLiteral("redacted"), event->redacted);
    details.insert(QStringLiteral("replyTargetId"), event->replyToEventId);
    details.insert(QStringLiteral("threadRootId"), event->threadRootId);
    details.insert(QStringLiteral("isThreadRoot"), event->isThreadRoot);
    details.insert(QStringLiteral("threadReplyCount"), event->threadReplyCount);
    return details;
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

bool TimelineModel::paginationFailed() const
{
    if (!m_client || m_roomId.isEmpty()) return false;
    return m_client->paginationFailed(m_roomId);
}

void TimelineModel::retrySend(int row)
{
    if (!m_client || m_roomId.isEmpty()) return;
    if (row < 0 || row >= m_events.size()) return;
    const auto &e = m_events.at(row);
    if (e.status != TimelineEvent::Failed || e.transactionId.isEmpty())
        return;
    m_client->retryFailedSend(m_roomId, e.transactionId);
}

void TimelineModel::retryDecryption()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    m_client->retryDecryption(m_roomId);
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
