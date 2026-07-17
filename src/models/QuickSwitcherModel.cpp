#include "models/QuickSwitcherModel.h"

#include "matrix/MatrixClient.h"
#include "spaces/SpaceManager.h"

#include <QCoreApplication>

#include <algorithm>

QuickSwitcherModel::QuickSwitcherModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void QuickSwitcherModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        // A logout empties the switcher so a stale result can never activate.
        connect(m_client, &MatrixClient::loggedOut, this,
                &QuickSwitcherModel::reset);
    }
    reset();
}

void QuickSwitcherModel::setSpaceManager(SpaceManager *spaces)
{
    m_spaces = spaces;
}

int QuickSwitcherModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_results.size());
}

QVariant QuickSwitcherModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= m_results.size())
        return {};
    const Result &r = m_results.at(index.row());
    switch (role) {
    case RoomIdRole:    return r.roomId;
    case NameRole:      return r.name;
    case SubtitleRole:  return r.subtitle;
    case CategoryRole:  return r.category;
    case AvatarUrlRole: return r.avatarUrl;
    case UnreadRole:    return r.unread;
    case HighlightRole: return r.highlight;
    case HasUnreadRole: return r.hasUnread;
    case IsSpaceRole:   return r.isSpace;
    case IsInviteRole:  return r.isInvite;
    default:            return {};
    }
}

QHash<int, QByteArray> QuickSwitcherModel::roleNames() const
{
    return {
        { RoomIdRole,    "roomId" },
        { NameRole,      "name" },
        { SubtitleRole,  "subtitle" },
        { CategoryRole,  "category" },
        { AvatarUrlRole, "avatarUrl" },
        { UnreadRole,    "unread" },
        { HighlightRole, "highlight" },
        { HasUnreadRole, "hasUnread" },
        { IsSpaceRole,   "isSpace" },
        { IsInviteRole,  "isInvite" },
    };
}

void QuickSwitcherModel::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();
    rebuild();
}

void QuickSwitcherModel::refresh()
{
    rebuild();
}

void QuickSwitcherModel::reset()
{
    const bool hadQuery = !m_query.isEmpty();
    m_query.clear();
    if (!m_results.isEmpty()) {
        beginResetModel();
        m_results.clear();
        endResetModel();
        Q_EMIT resultsChanged();
    }
    if (hadQuery)
        Q_EMIT queryChanged();
}

QVariantMap QuickSwitcherModel::resultAt(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= m_results.size())
        return map;
    const Result &r = m_results.at(row);
    map.insert(QStringLiteral("roomId"), r.roomId);
    map.insert(QStringLiteral("category"), r.category);
    map.insert(QStringLiteral("isSpace"), r.isSpace);
    map.insert(QStringLiteral("isInvite"), r.isInvite);
    return map;
}

int QuickSwitcherModel::matchScore(const QString &query, const QString &name,
                                   const QStringList &alternates)
{
    const QString q = query.trimmed().toLower();
    const QString lowerName = name.toLower();
    if (q.isEmpty())
        return 0;   // no query: everything matches with a neutral score.

    // Build the haystack (name + alternates) once.
    QString haystack = lowerName;
    for (const QString &alt : alternates) {
        haystack += QLatin1Char(' ');
        haystack += alt.toLower();
    }

    // Every whitespace-separated token must appear somewhere; a missing token
    // excludes the candidate entirely.
    const auto tokens = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (!haystack.contains(token))
            return -1;
    }

    // Relevance: a name prefix beats a name-word start beats any substring.
    int score = 100;
    if (lowerName.startsWith(q))
        score = 1000;
    else if (lowerName.contains(QLatin1Char(' ') + q))
        score = 500;
    else if (lowerName.contains(q))
        score = 300;
    return score;
}

void QuickSwitcherModel::rebuild()
{
    QList<Result> next;
    if (m_client) {
        const auto rooms = m_client->rooms();
        for (const auto &room : rooms) {
            if (room.membership != RoomInfo::Joined
                && room.membership != RoomInfo::Invited)
                continue;

            const bool isInvite = room.membership == RoomInfo::Invited;
            QStringList alternates;
            if (!room.canonicalAlias.isEmpty())
                alternates << room.canonicalAlias;
            if (room.isDirect && !room.directUserId.isEmpty())
                alternates << room.directUserId;

            const QString displayName =
                room.name.isEmpty() ? room.id : room.name;
            const int score = matchScore(m_query, displayName, alternates);
            if (score < 0)
                continue;   // filtered out

            Result r;
            r.roomId = room.id;
            r.name = displayName;
            r.avatarUrl = room.avatarUrl;
            r.unread = room.unreadCount;
            r.highlight = room.highlightCount;
            r.hasUnread = room.hasUnreadMessages || room.markedUnread
                          || room.unreadCount > 0;
            r.isSpace = room.isSpace;
            r.isInvite = isInvite;
            r.score = score;
            if (isInvite) {
                r.category = QStringLiteral("invite");
                r.subtitle = QCoreApplication::translate(
                    "QuickSwitcher", "Invitation");
            } else if (room.isSpace) {
                r.category = QStringLiteral("space");
                r.subtitle =
                    QCoreApplication::translate("QuickSwitcher", "Space");
            } else if (room.isDirect) {
                r.category = QStringLiteral("dm");
                r.subtitle = room.directUserId.isEmpty()
                    ? QCoreApplication::translate("QuickSwitcher",
                                                  "Direct message")
                    : room.directUserId;
            } else {
                r.category = QStringLiteral("room");
                r.subtitle = room.canonicalAlias;
            }
            next.append(std::move(r));
        }
    }

    std::sort(next.begin(), next.end(), [](const Result &a, const Result &b) {
        if (a.score != b.score)
            return a.score > b.score;
        // Invites surface above ordinary rooms at equal relevance.
        if (a.isInvite != b.isInvite)
            return a.isInvite;
        if (a.highlight != b.highlight)
            return a.highlight > b.highlight;
        if (a.hasUnread != b.hasUnread)
            return a.hasUnread;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    if (next.size() > kMaxResults)
        next.erase(next.begin() + kMaxResults, next.end());

    beginResetModel();
    m_results = std::move(next);
    endResetModel();
    Q_EMIT resultsChanged();
}
