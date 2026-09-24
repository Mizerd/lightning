#include "models/MentionSuggestionModel.h"

#include "matrix/MatrixClient.h"
#include "models/UserLookup.h"

#include <QSet>

#include <algorithm>

namespace {

QString localpartOf(const QString &userId)
{
    QString lp = userId;
    if (lp.startsWith(QLatin1Char('@')))
        lp = lp.mid(1);
    const int colon = lp.indexOf(QLatin1Char(':'));
    if (colon >= 0)
        lp = lp.left(colon);
    return lp;
}

} // namespace

MentionSuggestionModel::MentionSuggestionModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void MentionSuggestionModel::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::roomMembersReceived, this,
                &MentionSuggestionModel::onRoomMembersReceived);
        // The sync poke, not membersChanged: this model refetches on it, as
        // RoomInfoController does.
        connect(m_client, &MatrixClient::roomMemberEventSeen, this,
                &MentionSuggestionModel::onMembersChanged);
        connect(m_client, &MatrixClient::loggedOut, this,
                &MentionSuggestionModel::onLoggedOut);
    }
}

void MentionSuggestionModel::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    // A new room invalidates any in-flight request and the cached snapshot.
    m_membersOp = 0;
    m_all.clear();
    // Reset the @room permission to unknown (offered): it belonged to the
    // previous room, and the server refuses what the level does not allow.
    setRoomMentionAllowed(true);
    clearResults();
    Q_EMIT roomIdChanged();
    if (!m_roomId.isEmpty())
        requestMembers();
}

void MentionSuggestionModel::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    Q_EMIT queryChanged();
    rebuild(); // re-filter the cached members; never re-hits the server
}

void MentionSuggestionModel::requestMembers()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    const quint64 op = m_client->requestRoomMembers(m_roomId);
    if (op != 0)
        m_membersOp = op;
}

void MentionSuggestionModel::onRoomMembersReceived(quint64 opId,
                                                   const QString &roomId,
                                                   const QVariantMap &snapshot)
{
    if (opId != m_membersOp || roomId != m_roomId)
        return; // stale (old room / superseded request)
    // A partial (cache-only) snapshot is usable immediately, but the op
    // stays pending so the synced roster under the same op is not
    // dropped as stale.
    if (!snapshot.value(QStringLiteral("partial")).toBool())
        m_membersOp = 0;
    if (!snapshot.value(QStringLiteral("ok")).toBool())
        return;

    // Whether this account may notify the whole room comes from the same roster
    // snapshot, so @room is gated on the room the suggestions are for. Applied
    // only when the key is present: a missing key reads as false, which would
    // turn "backend never said" into "not allowed". Unknown means offered.
    if (snapshot.contains(QStringLiteral("canNotifyRoom")))
        setRoomMentionAllowed(
            snapshot.value(QStringLiteral("canNotifyRoom")).toBool());

    const QString ownId = m_client ? m_client->currentUserId() : QString();
    QList<Member> members;
    QSet<QString> seen;
    const QVariantList rows =
        snapshot.value(QStringLiteral("members")).toList();
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        const QString uid = row.value(QStringLiteral("userId")).toString();
        if (uid.isEmpty())
            continue;
        if (row.value(QStringLiteral("isOwn")).toBool() || uid == ownId)
            continue; // never suggest the signed-in user
        const QString membership =
            row.value(QStringLiteral("membership")).toString();
        // Allow-list: only joined or invited members are suggestable, and any
        // other label fails closed (the snapshot also carries banned members).
        // Rust says "joined"/"invited"; the mock uses raw "join"/"invite".
        if (membership != QLatin1String("joined")
            && membership != QLatin1String("join")
            && membership != QLatin1String("invited")
            && membership != QLatin1String("invite"))
            continue;
        if (seen.contains(uid))
            continue; // dedup by MXID
        seen.insert(uid);

        Member mem;
        mem.userId = uid;
        mem.rawDisplayName = row.value(QStringLiteral("displayName")).toString();
        mem.displayName = mem.rawDisplayName.isEmpty()
            ? matrix::user_lookup::localpartOrUserId(uid)
            : mem.rawDisplayName;
        mem.avatarMxc = row.value(QStringLiteral("avatarUrl")).toString();
        mem.role = row.value(QStringLiteral("role")).toString();
        mem.ambiguous = row.value(QStringLiteral("ambiguous")).toBool();
        members.append(mem);
    }
    m_all = members;
    rebuild();
}

void MentionSuggestionModel::onMembersChanged(const QString &roomId)
{
    // Membership changed for the open room: refresh unless a request is in
    // flight.
    if (roomId == m_roomId && !m_roomId.isEmpty() && m_membersOp == 0)
        requestMembers();
}

void MentionSuggestionModel::onLoggedOut()
{
    m_roomId.clear();
    m_query.clear();
    m_all.clear();
    m_membersOp = 0;
    clearResults();
}

int MentionSuggestionModel::matchScore(const QString &query,
                                       const QString &displayName,
                                       const QString &userId)
{
    const QString q = query.trimmed().toCaseFolded();
    if (q.isEmpty())
        return 1; // low base score: every member is a candidate

    const QString name = displayName.toCaseFolded();
    const QString localpart = localpartOf(userId).toCaseFolded();
    const QString mxid = userId.toCaseFolded();

    int best = -1;
    const auto consider = [&](const QString &hay, int prefixScore,
                              int subScore) {
        if (hay.isEmpty())
            return;
        if (hay.startsWith(q)) {
            best = qMax(best, prefixScore);
            return;
        }
        bool wordStart = false;
        const auto words = hay.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &w : words) {
            if (w.startsWith(q)) {
                wordStart = true;
                break;
            }
        }
        if (wordStart)
            best = qMax(best, prefixScore - 10);
        else if (hay.contains(q))
            best = qMax(best, subScore);
    };

    consider(name, 100, 40);
    consider(localpart, 90, 30);
    consider(mxid, 80, 20);
    return best;
}

// "@room" is offered for an empty query or any prefix of "room"; never via
// fuzzy scoring, so it is not surfaced by accident.
static bool matchesRoomMention(const QString &query)
{
    const QString q = query.trimmed().toLower();
    if (q.isEmpty())
        return true;
    return QStringLiteral("room").startsWith(q);
}

void MentionSuggestionModel::setRoomMentionAllowed(bool allowed)
{
    if (m_roomMentionAllowed == allowed)
        return;
    m_roomMentionAllowed = allowed;
    Q_EMIT roomMentionAllowedChanged();
    rebuild();
}

void MentionSuggestionModel::rebuild()
{
    struct Scored {
        const Member *member;
        int score;
    };
    QList<Scored> scored;
    scored.reserve(m_all.size());
    for (const Member &m : m_all) {
        const int s = matchScore(m_query, m.displayName, m.userId);
        if (s >= 0)
            scored.append({&m, s});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const Scored &a, const Scored &b) {
                         if (a.score != b.score)
                             return a.score > b.score;
                         return a.member->displayName.localeAwareCompare(
                                    b.member->displayName)
                             < 0;
                     });

    QList<Member> next;
    next.reserve(qMin(int(scored.size()), kMaxResults) + 1);
    // @room first when offered: it is the one non-person entry, and Element
    // lists it at the top too.
    if (m_roomMentionAllowed && matchesRoomMention(m_query)) {
        Member room;
        room.userId = QStringLiteral("@room");
        // "room", not "@room": the insertion builder adds the @.
        room.displayName = QStringLiteral("room");
        room.rawDisplayName = room.displayName;
        room.isRoom = true;
        next.append(room);
    }
    for (int i = 0; i < scored.size() && i < kMaxResults; ++i)
        next.append(*scored.at(i).member);

    beginResetModel();
    m_results = next;
    endResetModel();
    Q_EMIT countChanged();
}

void MentionSuggestionModel::clearResults()
{
    if (m_results.isEmpty())
        return;
    beginResetModel();
    m_results.clear();
    endResetModel();
    Q_EMIT countChanged();
}

int MentionSuggestionModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return int(m_results.size());
}

QVariant MentionSuggestionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_results.size())
        return {};
    const Member &m = m_results.at(index.row());
    switch (role) {
    case UserIdRole:
        return m.userId;
    case DisplayNameRole:
        return m.displayName;
    case AvatarMxcRole:
        return m.avatarMxc;
    case AmbiguousRole:
        return m.ambiguous;
    case RoleRole:
        return m.role;
    case IsRoomRole:
        return m.isRoom;
    default:
        return {};
    }
}

QHash<int, QByteArray> MentionSuggestionModel::roleNames() const
{
    return {
        { UserIdRole, "userId" },
        { DisplayNameRole, "displayName" },
        { AvatarMxcRole, "avatarMxc" },
        { AmbiguousRole, "ambiguous" },
        { RoleRole, "role" },
        { IsRoomRole, "isRoom" },
    };
}

QVariantMap MentionSuggestionModel::get(int row) const
{
    QVariantMap out;
    if (row < 0 || row >= m_results.size())
        return out;
    const Member &m = m_results.at(row);
    out.insert(QStringLiteral("userId"), m.userId);
    out.insert(QStringLiteral("displayName"), m.displayName);
    out.insert(QStringLiteral("avatarMxc"), m.avatarMxc);
    out.insert(QStringLiteral("ambiguous"), m.ambiguous);
    out.insert(QStringLiteral("role"), m.role);
    out.insert(QStringLiteral("isRoom"), m.isRoom);
    return out;
}
