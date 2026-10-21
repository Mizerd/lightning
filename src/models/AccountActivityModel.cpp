#include "models/AccountActivityModel.h"

#include <QDateTime>

#include <algorithm>

AccountActivityModel::AccountActivityModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AccountActivityModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

AccountActivityModel::Summary AccountActivityModel::visibleSummary(const Row &row)
{
    // A paused or non-live account shows nothing (design §8, DECISION §14.5):
    // its counts are as old as its last sync and would only mislead.
    if (row.liveState != Running)
        return {};
    return row.summary;
}

QVariant AccountActivityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    const Summary s = visibleSummary(row);
    switch (role) {
    case UserIdRole:
        return row.userId;
    case LiveStateRole:
        return int(row.liveState);
    case MentionCountRole:
        return s.mentions;
    case HasUnreadRole:
        return s.unreadRooms > 0 || s.invites > 0;
    case MentionsLowerBoundRole:
        return s.mentionsLowerBound;
    case UnreadRoomsRole:
        return s.unreadRooms;
    case InvitesRole:
        return s.invites;
    case UpdatedAtMsRole:
        return row.updatedAtMs;
    default:
        return {};
    }
}

QHash<int, QByteArray> AccountActivityModel::roleNames() const
{
    return {
        { UserIdRole, "userId" },
        { LiveStateRole, "liveState" },
        { MentionCountRole, "mentionCount" },
        { HasUnreadRole, "hasUnread" },
        { MentionsLowerBoundRole, "mentionsLowerBound" },
        { UnreadRoomsRole, "unreadRooms" },
        { InvitesRole, "invites" },
        { UpdatedAtMsRole, "updatedAtMs" },
    };
}

int AccountActivityModel::indexOf(const QString &uid) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).userId == uid)
            return i;
    }
    return -1;
}

int AccountActivityModel::insertRow(const QString &uid, quint64 epoch)
{
    const int at = int(m_rows.size());
    beginInsertRows(QModelIndex(), at, at);
    Row row;
    row.userId = uid;
    row.epoch = std::max(epoch, m_floor.value(uid, 0));
    m_rows.append(row);
    endInsertRows();
    return at;
}

qint64 AccountActivityModel::now() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

void AccountActivityModel::setClockForTest(std::function<qint64()> clock)
{
    m_clock = std::move(clock);
}

QVariantMap AccountActivityModel::activityOf(const Row *row)
{
    const Summary s = row ? visibleSummary(*row) : Summary{};
    QVariantMap m;
    m.insert(QStringLiteral("mentionCount"), s.mentions);
    m.insert(QStringLiteral("hasUnread"), s.unreadRooms > 0 || s.invites > 0);
    m.insert(QStringLiteral("mentionsLowerBound"), s.mentionsLowerBound);
    m.insert(QStringLiteral("invites"), s.invites);
    m.insert(QStringLiteral("liveState"), int(row ? row->liveState : NotLive));
    return m;
}

QVariantMap AccountActivityModel::activityFor(const QString &uid) const
{
    const int i = indexOf(uid);
    return activityOf(i < 0 ? nullptr : &m_rows.at(i));
}

void AccountActivityModel::emitRowChanged(int row)
{
    const QModelIndex idx = index(row, 0);
    Q_EMIT dataChanged(idx, idx);
}

bool AccountActivityModel::setSummary(const QString &uid, quint64 epoch,
                                      const Summary &s)
{
    // A removed account stays removed: a summary from the host retired with
    // it, at any epoch, must not bring the row back.
    if (uid.isEmpty() || m_removed.contains(uid))
        return false;
    int i = indexOf(uid);
    if (i < 0) {
        if (epoch < m_floor.value(uid, 0))
            return false;
        i = insertRow(uid, epoch);
    }
    Row &row = m_rows[i];
    if (epoch < row.epoch)
        return false;
    Summary clean = s;
    clean.unreadRooms = std::max(0, clean.unreadRooms);
    clean.mentions = std::max(0, clean.mentions);
    clean.invites = std::max(0, clean.invites);
    row.epoch = epoch;
    if (row.summary == clean)
        return true;
    const QVariantMap before = activityOf(&row);
    row.summary = clean;
    row.updatedAtMs = now();
    emitRowChanged(i);
    if (activityOf(&m_rows.at(i)) != before)
        Q_EMIT summaryChanged(uid);
    refreshTotals();
    return true;
}

void AccountActivityModel::setLiveState(const QString &uid, LiveState state)
{
    // The retiring host of a removed account still walks Stopping -> NotLive;
    // neither may re-create its row.
    if (uid.isEmpty() || m_removed.contains(uid))
        return;
    int i = indexOf(uid);
    if (i < 0) {
        if (state == NotLive)
            return; // nothing to say about an account without a row
        i = insertRow(uid, 0);
    }
    Row &row = m_rows[i];
    if (row.liveState == state)
        return;
    const QVariantMap before = activityOf(&row);
    row.liveState = state;
    emitRowChanged(i);
    Q_EMIT liveStateChanged(uid);
    // Running shows the summary and every other state hides it.
    if (activityOf(&m_rows.at(i)) != before)
        Q_EMIT summaryChanged(uid);
    refreshTotals();
}

void AccountActivityModel::resetAccount(const QString &uid, quint64 newEpoch)
{
    if (uid.isEmpty())
        return;
    const int i = indexOf(uid);
    if (i < 0) {
        m_floor.insert(uid, std::max(newEpoch, m_floor.value(uid, 0)));
        return;
    }
    Row &row = m_rows[i];
    if (newEpoch < row.epoch)
        return;
    row.epoch = newEpoch;
    if (row.summary == Summary{})
        return;
    const QVariantMap before = activityOf(&row);
    row.summary = Summary{};
    row.updatedAtMs = now();
    emitRowChanged(i);
    if (activityOf(&m_rows.at(i)) != before)
        Q_EMIT summaryChanged(uid);
    refreshTotals();
}

void AccountActivityModel::removeAccount(const QString &uid)
{
    if (uid.isEmpty())
        return;
    // Tombstoned whether or not a row exists: a Starting host that has not
    // reported yet must not create one afterwards either.
    m_removed.insert(uid);
    const int i = indexOf(uid);
    if (i < 0)
        return;
    const Row &row = m_rows.at(i);
    // AT the removed row's epoch, not past it. The tombstone already refuses
    // every input until the reopen, and the pool admits a new host only after
    // retiring the old one (which bumps the epoch), so nothing late can carry
    // this epoch. A "+1" would refuse reopenAccount(uid, epochFor(uid)) for
    // an account removed while not live, where nothing bumps the pool's epoch.
    m_floor.insert(uid, std::max(row.epoch, m_floor.value(uid, 0)));
    const bool showed = activityOf(&row) != activityOf(nullptr);
    beginRemoveRows(QModelIndex(), i, i);
    m_rows.removeAt(i);
    endRemoveRows();
    if (showed)
        Q_EMIT summaryChanged(uid);
    Q_EMIT liveStateChanged(uid);
    refreshTotals();
}

bool AccountActivityModel::reopenAccount(const QString &uid, quint64 epoch)
{
    if (uid.isEmpty())
        return false;
    if (!m_removed.contains(uid))
        return true;
    const quint64 floor = m_floor.value(uid, 0);
    if (epoch < floor)
        return false;
    m_removed.remove(uid);
    m_floor.insert(uid, epoch);
    return true;
}

void AccountActivityModel::setActiveUserId(const QString &uid)
{
    if (m_activeUserId == uid)
        return;
    m_activeUserId = uid;
    refreshTotals();
}

AccountActivityModel::Totals
AccountActivityModel::totalsExcluding(const QString &uid) const
{
    Totals t;
    for (const Row &row : m_rows) {
        if (row.userId == uid)
            continue;
        const Summary s = visibleSummary(row);
        t.unreadRooms += s.unreadRooms;
        t.mentions += s.mentions;
        if (s.unreadRooms > 0 || s.invites > 0 || s.mentions > 0)
            ++t.accountsWithActivity;
    }
    return t;
}

int AccountActivityModel::otherAccountsWithActivity() const
{
    return m_totals.accountsWithActivity;
}

AccountActivityModel::LiveState
AccountActivityModel::liveStateOf(const QString &uid) const
{
    const int i = indexOf(uid);
    return i < 0 ? NotLive : m_rows.at(i).liveState;
}

quint64 AccountActivityModel::epochOf(const QString &uid) const
{
    const int i = indexOf(uid);
    return i < 0 ? m_floor.value(uid, 0) : m_rows.at(i).epoch;
}

void AccountActivityModel::refreshTotals()
{
    const Totals t = totalsExcluding(m_activeUserId);
    if (t == m_totals)
        return;
    m_totals = t;
    Q_EMIT totalsChanged();
}
