#include "calls/CallShareModel.h"

#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <utility>

CallShareModel::CallShareModel(QObject *parent) : QAbstractListModel(parent)
{
}

int CallShareModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QHash<int, QByteArray> CallShareModel::roleNames() const
{
    return {
        { ShareIdRole, "shareId" },
        { OwnerIdentityRole, "ownerIdentity" },
        { OwnerDisplayNameRole, "ownerDisplayName" },
        { TrackKeyRole, "trackKey" },
        { LocalRole, "local" },
    };
}

QVariant CallShareModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const CallShareRow &row = m_rows.at(index.row());
    switch (role) {
    case ShareIdRole:
        return row.shareId;
    case OwnerIdentityRole:
        return row.ownerIdentity;
    case OwnerDisplayNameRole:
        return row.ownerDisplayName;
    case TrackKeyRole:
        return row.trackKey;
    case LocalRole:
        return row.local;
    default:
        return {};
    }
}

int CallShareModel::indexOf(const QString &shareId) const
{
    if (shareId.isEmpty())
        return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).shareId == shareId)
            return i;
    }
    return -1;
}

QString CallShareModel::ownerIdentityFor(const QString &shareId) const
{
    const int row = indexOfShare(shareId);
    return row < 0 ? QString() : m_rows.at(row).ownerIdentity;
}

int CallShareModel::indexOfShare(const QString &shareId) const
{
    return indexOf(shareId);
}

QVariantMap CallShareModel::get(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= m_rows.size())
        return out;
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        out.insert(QString::fromUtf8(it.value()), data(index(row), it.key()));
    return out;
}

QStringList CallShareModel::shareIds() const
{
    QStringList out;
    out.reserve(m_rows.size());
    for (const CallShareRow &row : m_rows)
        out.append(row.shareId);
    return out;
}

void CallShareModel::applyShares(const QVector<CallShareRow> &desired)
{
    const int before = m_rows.size();
    QStringList ended;
    QStringList appeared;

    {
        QSet<QString> wanted;
        wanted.reserve(desired.size());
        for (const CallShareRow &row : desired)
            wanted.insert(row.shareId);
        for (int i = m_rows.size() - 1; i >= 0; --i) {
            if (wanted.contains(m_rows.at(i).shareId))
                continue;
            ended.append(m_rows.at(i).shareId);
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.remove(i);
            endRemoveRows();
        }
    }

    for (int i = 0; i < desired.size(); ++i) {
        const CallShareRow &row = desired.at(i);
        if (row.shareId.isEmpty())
            continue;
        int at = -1;
        for (int j = i; j < m_rows.size(); ++j) {
            if (m_rows.at(j).shareId == row.shareId) {
                at = j;
                break;
            }
        }
        if (at < 0) {
            beginInsertRows(QModelIndex(), i, i);
            m_rows.insert(i, row);
            endInsertRows();
            appeared.append(row.shareId);
            continue;
        }
        if (at != i) {
            beginMoveRows(QModelIndex(), at, at, QModelIndex(), i);
            m_rows.move(at, i);
            endMoveRows();
        }
        QList<int> changed;
        CallShareRow &live = m_rows[i];
        if (live.ownerIdentity != row.ownerIdentity) {
            live.ownerIdentity = row.ownerIdentity;
            changed.append(OwnerIdentityRole);
        }
        if (live.ownerDisplayName != row.ownerDisplayName) {
            live.ownerDisplayName = row.ownerDisplayName;
            changed.append(OwnerDisplayNameRole);
        }
        // The track key legitimately fills in LATER: the SFU announces a
        // participant before it announces which track sid their share landed
        // on. A tile watches this role and re-attaches, which is why it must
        // be a dataChanged on an existing row rather than a remove/insert.
        if (live.trackKey != row.trackKey) {
            live.trackKey = row.trackKey;
            changed.append(TrackKeyRole);
        }
        if (live.local != row.local) {
            live.local = row.local;
            changed.append(LocalRole);
        }
        if (!changed.isEmpty())
            Q_EMIT dataChanged(index(i), index(i), changed);
    }

    // Announced AFTER the model settles, so a listener that reads the model
    // from the slot sees the finished state.
    for (const QString &id : std::as_const(ended))
        Q_EMIT shareEnded(id);
    for (const QString &id : std::as_const(appeared))
        Q_EMIT shareAppeared(id);
    if (m_rows.size() != before)
        Q_EMIT countChanged();
}

void CallShareModel::clear()
{
    if (m_rows.isEmpty())
        return;
    const QStringList ended = shareIds();
    beginRemoveRows(QModelIndex(), 0, m_rows.size() - 1);
    m_rows.clear();
    endRemoveRows();
    for (const QString &id : ended)
        Q_EMIT shareEnded(id);
    Q_EMIT countChanged();
}
