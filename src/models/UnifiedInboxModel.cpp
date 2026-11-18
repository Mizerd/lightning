#include "models/UnifiedInboxModel.h"

#include "models/AccountActivityModel.h"

UnifiedInboxModel::UnifiedInboxModel(AccountActivityModel *activity)
    : QAbstractListModel(activity)
    , m_activity(activity)
{
    if (m_activity) {
        connect(m_activity, &AccountActivityModel::liveStateChanged, this,
                [this](const QString &uid) {
            if (m_activity->liveStateOf(uid) != AccountActivityModel::Running)
                dropAccount(uid);
        });
    }
}

void UnifiedInboxModel::setEpochSource(std::function<quint64(const QString &)> epochOf)
{
    m_epochOf = std::move(epochOf);
}

int UnifiedInboxModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant UnifiedInboxModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case AccountUserIdRole: return e.accountUserId;
    case AccountLabelRole: return e.accountLabel;
    case RoomIdRole: return e.roomId;
    case RoomNameRole: return e.roomName;
    case SenderIdRole: return e.senderId;
    case SenderNameRole: return e.senderName;
    case PreviewRole: return e.preview;
    case UndecryptableRole: return e.undecryptable;
    case TimestampMsRole: return e.timestampMs;
    case EventIdRole: return e.eventId;
    case ThreadRootIdRole: return e.threadRootId;
    case HighlightRole: return e.highlight;
    default: return {};
    }
}

QHash<int, QByteArray> UnifiedInboxModel::roleNames() const
{
    return {
        { AccountUserIdRole, "accountUserId" },
        { AccountLabelRole, "accountLabel" },
        { RoomIdRole, "roomId" },
        { RoomNameRole, "roomName" },
        { SenderIdRole, "senderId" },
        { SenderNameRole, "senderName" },
        { PreviewRole, "preview" },
        { UndecryptableRole, "undecryptable" },
        { TimestampMsRole, "timestampMs" },
        { EventIdRole, "eventId" },
        { ThreadRootIdRole, "threadRootId" },
        { HighlightRole, "highlight" },
    };
}

bool UnifiedInboxModel::accepts(const QString &uid, quint64 epoch) const
{
    if (uid.isEmpty() || !m_activity || m_activity->isRemoved(uid)
        || uid == m_activity->activeUserId()
        || m_activity->liveStateOf(uid) != AccountActivityModel::Running)
        return false;
    return !m_epochOf || m_epochOf(uid) == epoch;
}

bool UnifiedInboxModel::add(quint64 epoch, const Entry &entry)
{
    if (entry.roomId.isEmpty() || entry.eventId.isEmpty()
        || !accepts(entry.accountUserId, epoch))
        return false;
    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &old = m_entries[i];
        if (old.accountUserId == entry.accountUserId && old.eventId == entry.eventId) {
            old = entry;
            const QModelIndex idx = index(i, 0);
            Q_EMIT dataChanged(idx, idx);
            return true;
        }
    }
    int at = 0;
    while (at < m_entries.size() && m_entries.at(at).timestampMs >= entry.timestampMs)
        ++at;
    if (at >= kMaxEntries)
        return false;
    beginInsertRows(QModelIndex(), at, at);
    m_entries.insert(at, entry);
    endInsertRows();
    if (m_entries.size() > kMaxEntries) {
        const int last = int(m_entries.size()) - 1;
        beginRemoveRows(QModelIndex(), last, last);
        m_entries.removeLast();
        endRemoveRows();
    }
    Q_EMIT countChanged();
    return true;
}

void UnifiedInboxModel::dropWhere(const std::function<bool(const Entry &)> &drop)
{
    bool removed = false;
    for (int i = int(m_entries.size()) - 1; i >= 0; --i) {
        if (!drop(m_entries.at(i)))
            continue;
        beginRemoveRows(QModelIndex(), i, i);
        m_entries.removeAt(i);
        endRemoveRows();
        removed = true;
    }
    if (removed)
        Q_EMIT countChanged();
}

void UnifiedInboxModel::dropAccount(const QString &uid)
{
    dropWhere([&uid](const Entry &e) { return e.accountUserId == uid; });
}

void UnifiedInboxModel::dropRoom(const QString &uid, const QString &roomId)
{
    dropWhere([&](const Entry &e) { return e.accountUserId == uid && e.roomId == roomId; });
}

void UnifiedInboxModel::dropEvent(const QString &uid, const QString &eventId)
{
    dropWhere([&](const Entry &e) { return e.accountUserId == uid && e.eventId == eventId; });
}

QVariantMap UnifiedInboxModel::entryAt(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= m_entries.size())
        return m;
    const QHash<int, QByteArray> names = roleNames();
    const QModelIndex idx = index(row, 0);
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(idx, it.key()));
    return m;
}
