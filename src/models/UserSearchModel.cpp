#include "models/UserSearchModel.h"

#include "matrix/MatrixClient.h"
#include "models/UserLookup.h"

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
        connect(m_client, &MatrixClient::userProfileFinished,
                this, &UserSearchModel::onProfileFinished);
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

void UserSearchModel::invalidatePending()
{
    // Any in-flight completion is stale the moment the query changes.
    m_pendingOp = 0;
    m_pendingProfileOp = 0;
    m_directoryDone = false;
    m_directoryOk = true;
    m_directoryResults.clear();
    m_candidateUserId.clear();
    m_candidateNamesServer = false;
    m_candidateConfirmed = false;
    m_candidateDisplayName.clear();
    m_candidateAvatarUrl.clear();
}

void UserSearchModel::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();

    invalidatePending();

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
    invalidatePending();
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

    // v0.5.11: exact candidate against the account's own server (bare
    // localpart) or the explicitly named server. Confirmed via profile
    // lookup; bare-localpart candidates surface only after confirmation.
    const QString ownUser = m_client->currentUserId();
    const QString ownServer =
        matrix::user_lookup::serverNameFromUserId(ownUser);
    const QString candidate =
        matrix::user_lookup::exactCandidate(trimmed, ownServer);
    if (!candidate.isEmpty() && candidate != ownUser) {
        m_candidateUserId = candidate;
        m_candidateNamesServer = matrix::user_lookup::queryNamesServer(trimmed);
        // 0 = backend without profile lookup: typed-MXID rows still appear
        // unconfirmed below; bare-localpart candidates stay hidden.
        m_pendingProfileOp = m_client->fetchUserProfile(candidate);
    }

    setState(QStringLiteral("loading"));
    rebuildRows();
    updateStateFromResults();
}

QList<UserSearchModel::Result>
UserSearchModel::mergeResults(const QList<Result> &exact,
                              const QList<Result> &directory,
                              const QString &ownUser)
{
    QList<Result> merged;
    const auto add = [&merged, &ownUser](const Result &row) {
        if (row.userId.isEmpty() || row.userId == ownUser)
            return;
        for (Result &existing : merged) {
            if (existing.userId != row.userId)
                continue;
            // Duplicate: keep the first row's provenance, fill in missing
            // presentation fields from the later source.
            if (existing.displayName.isEmpty())
                existing.displayName = row.displayName;
            if (existing.avatarUrl.isEmpty())
                existing.avatarUrl = row.avatarUrl;
            return;
        }
        merged.append(row);
    };
    for (const Result &row : exact)
        add(row);
    for (const Result &row : directory)
        add(row);
    return merged;
}

void UserSearchModel::rebuildRows()
{
    QList<Result> exact;
    if (!m_candidateUserId.isEmpty()) {
        const bool offerUnconfirmed = m_candidateNamesServer;
        if (m_candidateConfirmed || offerUnconfirmed) {
            Result row;
            row.userId = m_candidateUserId;
            row.displayName = m_candidateDisplayName;
            row.avatarUrl = m_candidateAvatarUrl;
            row.isExactMxid = true;
            row.source = m_candidateNamesServer
                             ? QStringLiteral("exact_mxid")
                             : QStringLiteral("exact_local");
            exact.append(row);
        }
    }

    const QString ownUser = m_client ? m_client->currentUserId() : QString();
    const QList<Result> next = mergeResults(exact, m_directoryResults, ownUser);

    beginResetModel();
    m_results = next;
    endResetModel();
}

void UserSearchModel::updateStateFromResults()
{
    if (!m_results.isEmpty()) {
        setState(QStringLiteral("results"));
        return;
    }
    if (m_pendingOp != 0 || m_pendingProfileOp != 0) {
        setState(QStringLiteral("loading"));
        return;
    }
    if (m_directoryDone && !m_directoryOk)
        setState(QStringLiteral("error"));
    else
        setState(QStringLiteral("no_results"));
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
    m_directoryDone = true;
    m_directoryOk = ok;

    m_directoryResults.clear();
    if (ok) {
        for (const QVariant &value : results) {
            const QVariantMap row = value.toMap();
            Result r;
            r.userId = row.value(QStringLiteral("userId")).toString();
            r.displayName = row.value(QStringLiteral("displayName")).toString();
            r.avatarUrl = row.value(QStringLiteral("avatarUrl")).toString();
            if (!r.userId.isEmpty())
                m_directoryResults.append(r);
        }
    }

    rebuildRows();
    updateStateFromResults();
}

void UserSearchModel::onProfileFinished(quint64 opId, bool ok,
                                        const QString &userId,
                                        const QString &displayName,
                                        const QString &avatarUrl,
                                        const QString &category)
{
    Q_UNUSED(category);
    if (opId != m_pendingProfileOp || m_pendingProfileOp == 0)
        return;
    m_pendingProfileOp = 0;
    if (userId != m_candidateUserId)
        return; // candidate changed while the lookup was in flight

    if (ok) {
        // The homeserver confirmed the user exists. A failed lookup keeps
        // bare-localpart candidates hidden (nothing is invented); typed
        // full ids stay offered unconfirmed for federated invites.
        m_candidateConfirmed = true;
        m_candidateDisplayName = displayName;
        m_candidateAvatarUrl = avatarUrl;
    }

    rebuildRows();
    updateStateFromResults();
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
    case SourceRole:      return r.source;
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
        { SourceRole,      "source" },
    };
}
