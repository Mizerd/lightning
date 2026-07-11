#include "models/UserSearchModel.h"

#include "matrix/MatrixClient.h"

#include <QRegularExpression>
#include <QVariantMap>

namespace {
constexpr int kMinQueryLength = 2;
constexpr int kResultLimit = 20;
} // namespace

UserSearchModel::UserSearchModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &UserSearchModel::dispatchSearch);
}

void UserSearchModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::userSearchFinished,
                this, &UserSearchModel::onSearchFinished);
        connect(m_client, &MatrixClient::loggedOut, this, [this]() { clear(); });
    }
    Q_EMIT stateChanged();
}

bool UserSearchModel::supported() const
{
    return m_client && m_client->supportsRoomManagement();
}

bool UserSearchModel::looksLikeMxid(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral("^@[^:@\\s]+:[A-Za-z0-9.\\-]+(?::\\d+)?$"));
    return re.match(text.trimmed()).hasMatch();
}

void UserSearchModel::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();

    // Invalidate any in-flight search immediately: its completion is stale.
    m_pendingOp = 0;

    const QString trimmed = m_query.trimmed();
    if (trimmed.length() < kMinQueryLength) {
        beginResetModel();
        m_results.clear();
        endResetModel();
        m_debounce.stop();
        setState(QStringLiteral("idle"));
        return;
    }
    setState(QStringLiteral("loading"));
    m_debounce.start();
}

void UserSearchModel::clear()
{
    m_debounce.stop();
    m_pendingOp = 0;
    m_query.clear();
    beginResetModel();
    m_results.clear();
    endResetModel();
    Q_EMIT queryChanged();
    setState(QStringLiteral("idle"));
}

QString UserSearchModel::userIdAt(int row) const
{
    return (row >= 0 && row < m_results.size()) ? m_results.at(row).userId
                                                : QString();
}

QString UserSearchModel::displayNameAt(int row) const
{
    return (row >= 0 && row < m_results.size()) ? m_results.at(row).displayName
                                                : QString();
}

void UserSearchModel::dispatchSearch()
{
    if (!m_client)
        return;
    const QString trimmed = m_query.trimmed();
    if (trimmed.length() < kMinQueryLength)
        return;
    const quint64 opId = m_client->searchUsers(trimmed, kResultLimit);
    if (opId == 0) {
        setState(QStringLiteral("error"));
        return;
    }
    m_pendingOp = opId;
    setState(QStringLiteral("loading"));
}

void UserSearchModel::onSearchFinished(quint64 opId, bool ok,
                                       const QVariantList &results,
                                       bool limited, const QString &category)
{
    Q_UNUSED(limited);
    Q_UNUSED(category);
    // Stale-by-generation: only the most recently dispatched operation may
    // populate the model. Older completions are dropped silently.
    if (opId != m_pendingOp || m_pendingOp == 0)
        return;
    m_pendingOp = 0;

    const QString ownUser = m_client ? m_client->currentUserId() : QString();
    QList<Result> next;
    QStringList seen;

    // A complete typed MXID is always offered, first, even when the
    // directory does not know it.
    const QString trimmed = m_query.trimmed();
    if (looksLikeMxid(trimmed) && trimmed != ownUser) {
        Result exact;
        exact.userId = trimmed;
        exact.isExactMxid = true;
        next.append(exact);
        seen.append(trimmed);
    }

    if (ok) {
        for (const QVariant &value : results) {
            const QVariantMap row = value.toMap();
            const QString userId = row.value(QStringLiteral("userId")).toString();
            if (userId.isEmpty() || seen.contains(userId) || userId == ownUser)
                continue;
            seen.append(userId);
            Result r;
            r.userId = userId;
            r.displayName = row.value(QStringLiteral("displayName")).toString();
            r.avatarUrl = row.value(QStringLiteral("avatarUrl")).toString();
            next.append(r);
        }
    }

    beginResetModel();
    m_results = next;
    endResetModel();

    if (!ok && next.isEmpty())
        setState(QStringLiteral("error"));
    else if (next.isEmpty())
        setState(QStringLiteral("no_results"));
    else
        setState(QStringLiteral("results"));
}

void UserSearchModel::setState(const QString &state)
{
    if (m_state == state) {
        Q_EMIT stateChanged(); // count may still have changed
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}

int UserSearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_results.size());
}

QVariant UserSearchModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_results.size())
        return {};
    const Result &r = m_results.at(index.row());
    switch (role) {
    case UserIdRole:      return r.userId;
    case DisplayNameRole: return r.displayName;
    case AvatarUrlRole:   return r.avatarUrl;
    case IsExactMxidRole: return r.isExactMxid;
    default:              return {};
    }
}

QHash<int, QByteArray> UserSearchModel::roleNames() const
{
    return {
        { UserIdRole,      "userId" },
        { DisplayNameRole, "displayName" },
        { AvatarUrlRole,   "avatarUrl" },
        { IsExactMxidRole, "isExactMxid" },
    };
}
