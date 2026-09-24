#include "models/TimelineModel.h"

#include "matrix/MatrixClient.h"
#include "models/MessageHtml.h"
#include "models/UserLookup.h"
#include "profile/UserProfileResolver.h"

#include <QRegularExpression>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>

namespace {
// Element/Discord-style visual grouping window. Five minutes is short enough
// to keep conversations scannable while suppressing repetitive identity.
constexpr qint64 kSenderGroupThresholdSeconds = 5 * 60;

// Room id for member lookups. Thread-timeline events carry the composite
// timeline id in `roomId`, which no member cache, receipt or typing set is
// keyed by, so lookups must use the real room. Identity for an ordinary room.
inline QString memberLookupRoomId(const QString &id)
{
    return MatrixClient::threadTimelineRoomId(id);
}
}

TimelineModel::TimelineModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Recompute on countChanged rather than at each insertion site, so a new
    // path cannot forget it. modelReset is hooked separately because a reset
    // does not always move the count.
    connect(this, &TimelineModel::countChanged,
            this, &TimelineModel::refreshLatestCallEvent);
    connect(this, &TimelineModel::modelReset,
            this, &TimelineModel::refreshLatestCallEvent);
    // Keep an active in-timeline search in sync with timeline changes. O(n)
    // over the bounded loaded set, and only while a search is active.
    const auto resync = [this] {
        if (!m_searchActive)
            return;
        const QString before = searchCurrentEventId();
        const int beforeCount = m_searchResults.size();
        recomputeSearch();
        if (searchCurrentEventId() != before
            || m_searchResults.size() != beforeCount)
            Q_EMIT searchChanged();
    };
    connect(this, &QAbstractItemModel::rowsInserted, this, resync);
    connect(this, &QAbstractItemModel::rowsRemoved, this, resync);
    connect(this, &QAbstractItemModel::modelReset, this, resync);
    connect(this, &QAbstractItemModel::dataChanged, this, resync);

}

QString TimelineModel::senderDisplayName(const TimelineEvent &event) const
{
    // Fallback order: the room-specific name carried on the event, the backend
    // member lookup, then the localpart. The full MXID is never the visible
    // label; it stays available for tooltips, details and disambiguation.
    if (!event.senderDisplayName.isEmpty())
        return event.senderDisplayName;
    if (m_client) {
        const QString display = m_client->displayNameFor(
            memberLookupRoomId(event.roomId), event.sender);
        // Backends return the raw user id when nothing is known: unresolved,
        // not a display name.
        if (!display.isEmpty() && display != event.sender)
            return display;
    }
    return matrix::user_lookup::localpartOrUserId(event.sender);
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

namespace {
/// Whether a call row states video intent. The typed field is authoritative;
/// older rows carry only the legacy "m.call.video" state kind. False means
/// "not known to be video", not "audio only".
bool callRowIsVideo(const TimelineEvent &e)
{
    return e.callIsVideo || e.stateKind == QLatin1String("m.call.video");
}

/// One gallery attachment as the QML-facing map GalleryItemsRole promises.
QVariantMap galleryItemVariant(const GalleryItem &g)
{
    QVariantMap item;
    item.insert(QStringLiteral("mediaKey"), g.mediaKey);
    item.insert(QStringLiteral("kind"), g.kind);
    item.insert(QStringLiteral("filename"), g.filename);
    item.insert(QStringLiteral("mimetype"), g.mimetype);
    item.insert(QStringLiteral("size"), g.size);
    item.insert(QStringLiteral("width"), g.width);
    item.insert(QStringLiteral("height"), g.height);
    item.insert(QStringLiteral("durationMs"), g.durationMs);
    item.insert(QStringLiteral("thumbAvailable"), g.thumbAvailable);
    return item;
}
} // namespace

// A call row in either shape: the Rust bridge's `msgtype: "call"`
// (TimelineEvent::CallEvent), or a legacy state event with kind "m.call",
// which the mock/HTTP backends and cached timelines still produce. Both must
// render as calls rather than join a "N room updates" group.
bool TimelineModel::isCallEventRow(const TimelineEvent &e)
{
    if (e.type == TimelineEvent::CallEvent)
        return true;
    return e.type == TimelineEvent::StateChange
        && (e.stateKind == QLatin1String("m.call")
            || e.stateKind == QLatin1String("m.call.video"));
}

void TimelineModel::refreshLatestCallEvent()
{
    // Newest end first, stopping at the first call row.
    QString found;
    for (auto it = m_events.crbegin(); it != m_events.crend(); ++it) {
        if (!isCallEventRow(*it))
            continue;
        found = it->eventId;
        break;
    }
    if (found == m_latestCallEventId)
        return;
    m_latestCallEventId = found;
    Q_EMIT latestCallEventIdChanged();
}

bool TimelineModel::isVisualMessage(const TimelineEvent &event) const
{
    // Call rows break sender groups like state rows: they carry no sender
    // header, and two messages from one person around a call must not stay one
    // block.
    return !event.isVirtual() && event.type != TimelineEvent::StateChange
        && !isCallEventRow(event);
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

bool TimelineModel::groupingInputsDiffer(const TimelineEvent &before,
                                         const TimelineEvent &after) const
{
    // The only per-event inputs sender and state grouping read; profile, body,
    // media, reaction and decryption updates do not force a regroup.
    return before.sender != after.sender
        || before.timestamp != after.timestamp
        || before.type != after.type
        || before.redacted != after.redacted
        || before.stateKind != after.stateKind;
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
        // Forwarded for this timeline's real room only (a thread timeline's id
        // is the composite; the answer names the room).
        connect(m_client, &MatrixClient::editHistoryReceived, this,
                [this](const QString &roomId, const QString &eventId, bool ok,
                       bool partial, const QVariantList &revisions) {
            if (roomId != m_realRoomId)
                return;
            Q_EMIT editHistoryReceived(eventId, ok, partial, revisions);
        });
        connect(m_client, &MatrixClient::eventSourceReceived, this,
                [this](const QString &roomId, const QString &eventId, bool ok,
                       const QString &json, const QVariantMap &encryption) {
            if (roomId != m_realRoomId)
                return;
            Q_EMIT eventSourceReceived(eventId, ok, json, encryption);
        });
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
        connect(m_client, &MatrixClient::eventsInsertedAt,
                this, &TimelineModel::onEventsInsertedAt);
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

void TimelineModel::setProfileResolver(UserProfileResolver *resolver)
{
    if (m_profiles == resolver)
        return;
    if (m_profiles)
        m_profiles->disconnect(this);
    m_profiles = resolver;
    if (m_profiles) {
        connect(m_profiles, &UserProfileResolver::resolved, this,
                [this](const QString &, const QString &, const QString &) {
                    refreshStaleMentionRows();
                });
    }
}

QString TimelineModel::mentionNameFor(const QString &userId, bool ask) const
{
    // m_realRoomId, never m_roomId: a lookup against a thread's composite id
    // always misses.
    QString name = m_client ? m_client->displayNameFor(m_realRoomId, userId)
                            : QString();
    // Backends return the raw user id when nothing is known: unresolved, not a
    // display name.
    if (name == userId)
        name.clear();
    if (name.isEmpty() && m_profiles) {
        const UserProfileResolver::Profile p = m_profiles->profile(userId);
        if (p.known && !p.displayName.isEmpty())
            name = p.displayName;
        else if (ask && !p.known)
            m_profiles->request(userId);
    }
    return name;
}

void TimelineModel::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    // Resolved once per binding. In a thread model m_roomId is the composite
    // `room ␟ thread ␟ root`, which no room-keyed lookup matches; every lookup
    // uses this instead. Equal to m_roomId for an ordinary room.
    m_realRoomId = MatrixClient::threadTimelineRoomId(roomId);
    Q_EMIT roomIdChanged();
    // A room/thread switch clears search state; never carry a query or its
    // plaintext matches across timelines.
    endSearch();
    // Spoiler reveals belong to the timeline they were made in. Theme changes
    // and member hydration do not clear them.
    m_spoilersRevealed.clear();
    reload();
    refreshTypingText();
    Q_EMIT paginationChanged();
}

void TimelineModel::beginSearch(const QString &query)
{
    m_searchActive = true;
    m_searchQuery = query;
    recomputeSearch();
    // Start at the newest match, closest to where the user is reading.
    m_searchIndex = m_searchResults.isEmpty()
                        ? -1
                        : static_cast<int>(m_searchResults.size()) - 1;
    Q_EMIT searchChanged();
}

void TimelineModel::updateSearch(const QString &query)
{
    if (!m_searchActive) {
        beginSearch(query);
        return;
    }
    if (query == m_searchQuery)
        return;
    m_searchQuery = query;
    recomputeSearch();
    m_searchIndex = m_searchResults.isEmpty()
                        ? -1
                        : static_cast<int>(m_searchResults.size()) - 1;
    Q_EMIT searchChanged();
}

void TimelineModel::searchNext()
{
    if (m_searchResults.isEmpty())
        return;
    m_searchIndex = (m_searchIndex + 1) % m_searchResults.size();
    Q_EMIT searchChanged();
}

void TimelineModel::searchPrev()
{
    if (m_searchResults.isEmpty())
        return;
    const int n = static_cast<int>(m_searchResults.size());
    m_searchIndex = (m_searchIndex - 1 + n) % n;
    Q_EMIT searchChanged();
}

void TimelineModel::endSearch()
{
    if (!m_searchActive && m_searchQuery.isEmpty() && m_searchResults.isEmpty()
        && m_searchIndex < 0)
        return;
    m_searchActive = false;
    m_searchQuery.clear();
    m_searchResults.clear();
    m_searchIndex = -1;
    Q_EMIT searchChanged();
}

void TimelineModel::recomputeSearch()
{
    // Keep the selected match across a recompute when it still matches.
    const QString wasSelected = (m_searchIndex >= 0
                                 && m_searchIndex < m_searchResults.size())
                                    ? m_searchResults.at(m_searchIndex)
                                    : QString{};
    m_searchResults.clear();
    const QString needle = m_searchQuery.trimmed();
    if (m_searchActive && !needle.isEmpty()) {
        for (int raw = 0; raw < m_events.size(); ++raw) {
            const auto &event = m_events.at(raw);
            if (event.isVirtual() || event.eventId.isEmpty())
                continue;
            // Read the in-hand event's visible text directly (same rules as
            // visibleTextForEvent) to avoid a per-row id lookup, which made
            // this quadratic.
            if (event.type == TimelineEvent::StateChange || event.redacted)
                continue;
            if (event.body.contains(needle, Qt::CaseInsensitive))
                m_searchResults.append(event.eventId);
        }
    }
    if (m_searchResults.isEmpty()) {
        m_searchIndex = -1;
        return;
    }
    const int keep = m_searchResults.indexOf(wasSelected);
    m_searchIndex = keep >= 0 ? keep
                              : static_cast<int>(m_searchResults.size()) - 1;
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

QString TimelineModel::memberDisplayName(const QString &roomId,
                                        const QString &userId) const
{
    if (userId.isEmpty())
        return {};
    if (m_client) {
        const QString resolved = m_client->displayNameFor(roomId, userId);
        // Backends return the raw user id when nothing is known: unresolved,
        // not a display name.
        if (!resolved.isEmpty() && resolved != userId)
            return resolved;
    }
    return matrix::user_lookup::localpartOrUserId(userId);
}

QVariantList TimelineModel::reactionsVariant(const TimelineEvent &e) const
{
    const QString lookupRoom = memberLookupRoomId(e.roomId);
    QVariantList out;
    out.reserve(e.reactions.size());
    for (const auto &r : e.reactions) {
        QVariantMap m;
        m.insert(QStringLiteral("key"),   r.key);
        m.insert(QStringLiteral("count"), r.count);
        m.insert(QStringLiteral("byMe"),  r.byMe);
        // Reactors arrive as a bounded window of user ids (16, the receipt cap)
        // and are resolved here through the normal member lookup, never a
        // network call from a role. `reactorTotal` is the uncapped count for
        // "and N more".
        QStringList names;
        names.reserve(r.senders.size());
        for (const QString &userId : r.senders) {
            const QString name = memberDisplayName(lookupRoom, userId);
            if (!name.isEmpty())
                names.append(name);
        }
        m.insert(QStringLiteral("reactorNames"),  names);
        m.insert(QStringLiteral("reactorTotal"),  r.count);
        out.append(m);
    }
    return out;
}

QVariantList TimelineModel::pollAnswersVariant(const TimelineEvent &e) const
{
    QVariantList out;
    out.reserve(e.pollAnswers.size());
    for (const auto &a : e.pollAnswers) {
        QVariantMap m;
        m.insert(QStringLiteral("id"),    a.id);
        m.insert(QStringLiteral("text"),  a.text);
        m.insert(QStringLiteral("count"), a.count);
        m.insert(QStringLiteral("byMe"),  a.byMe);
        out.append(m);
    }
    return out;
}

QVariantList TimelineModel::readReceiptsVariant(const TimelineEvent &e) const
{
    // Element convention: only the user's own receipt is hidden. Everyone
    // else's marker renders wherever it points, including on their own message
    // (the SDK's implicit sender receipt), which is how a DM shows "read up to
    // here". This is one presentation rule for every backend. Newest readers
    // first so the bounded chip stack and "+N" show the most recent. Names
    // resolve like senderDisplayName(): member lookup, then the localpart.
    QList<ReadReceipt> receipts;
    receipts.reserve(e.readBy.size());
    for (const auto &r : e.readBy) {
        if (r.userId != m_selfUserId)
            receipts.append(r);
    }
    std::stable_sort(receipts.begin(), receipts.end(),
                     [](const ReadReceipt &a, const ReadReceipt &b) {
                         return a.tsMs > b.tsMs;
                     });
    const QString lookupRoom = memberLookupRoomId(e.roomId);
    QVariantList out;
    out.reserve(receipts.size());
    for (const auto &r : receipts) {
        const QString display = memberDisplayName(lookupRoom, r.userId);
        QString avatar =
            m_client ? m_client->avatarMxcFor(lookupRoom, r.userId) : QString{};
        // Member-cache miss: fall back to the avatar this timeline has seen on
        // the reader's own messages.
        if (avatar.isEmpty())
            avatar = m_senderAvatarIndex.value(r.userId);
        QVariantMap m;
        m.insert(QStringLiteral("userId"),      r.userId);
        m.insert(QStringLiteral("displayName"), display);
        m.insert(QStringLiteral("avatarMxc"),   avatar);
        m.insert(QStringLiteral("tsMs"),        r.tsMs);
        out.append(m);
    }
    return out;
}

// Virtual rows (date dividers, read markers, timeline start) are SDK
// bookkeeping interleaved freely between events. They must not split a run of
// state events; only a visible message/media event or the timeline edge ends
// a group.
namespace {
/// Whether a room-state kind is MatrixRTC call membership. Every join,
/// refresh and leave writes one; they are hidden from the timeline, as in
/// Element, since the call tile, banner and notification convey the call.
///
/// Covers the legacy session format, the sticky MSC4143 format and the stable
/// names both are heading for; the set is deliberately wide.
bool isRtcMembershipKind(const QString &kind)
{
    return kind == QLatin1String("org.matrix.msc3401.call.member")
        || kind == QLatin1String("org.matrix.msc4143.rtc.member")
        || kind == QLatin1String("m.call.member")
        || kind == QLatin1String("m.rtc.member");
}
} // namespace

int TimelineModel::stateGroupLeaderRow(int row) const
{
    if (row < 0 || row >= m_events.size()
        || m_events.at(row).type != TimelineEvent::StateChange
        || isCallEventRow(m_events.at(row)))
        return -1;

    int leader = row;
    int probe = row - 1;
    while (probe >= 0) {
        const auto &e = m_events.at(probe);
        // A legacy call row (state kind "m.call") draws its own tile, so it
        // ends the run like a message; otherwise it would lead the group and
        // draw the summary over its tile.
        if (e.type == TimelineEvent::StateChange && !isCallEventRow(e)) {
            leader = probe;
            --probe;
            continue;
        }
        // A date divider ends the run so one collapsed group never spans days
        // under a single date separator. Read markers and the timeline-start
        // row stay transparent.
        if (e.type == TimelineEvent::DateDivider)
            break;
        if (e.isVirtual()) {
            --probe;
            continue;
        }
        break; // a visible message/media/call event ends the group
    }
    return leader;
}

// Same shape as stateGroupLeaderRow: transparent through read markers and the
// timeline-start row, broken by a date divider or a visible row.
int TimelineModel::deletedGroupLeaderRow(int row) const
{
    if (row < 0 || row >= m_events.size() || !m_events.at(row).redacted)
        return -1;

    int leader = row;
    int probe = row - 1;
    while (probe >= 0) {
        const auto &e = m_events.at(probe);
        if (e.redacted) {
            leader = probe;
            --probe;
            continue;
        }
        if (e.type == TimelineEvent::DateDivider)
            break;
        if (e.isVirtual()) {
            --probe;
            continue;
        }
        break;
    }
    return leader;
}

int TimelineModel::deletedGroupLengthFrom(int leaderRow) const
{
    if (leaderRow < 0 || leaderRow >= m_events.size())
        return 0;
    int count = 0;
    for (int row = leaderRow; row < m_events.size(); ++row) {
        const auto &e = m_events.at(row);
        if (e.redacted) {
            ++count;
            continue;
        }
        if (e.type == TimelineEvent::DateDivider)
            break;
        if (e.isVirtual())
            continue;
        break;
    }
    return count;
}

int TimelineModel::readMarkerRow() const
{
    // The SDK inserts exactly one of these, so direction does not matter.
    for (int row = 0; row < m_events.size(); ++row) {
        if (m_events.at(row).type == TimelineEvent::ReadMarker)
            return row;
    }
    return -1;
}

int TimelineModel::stateActivityRowCount() const
{
    int count = 0;
    for (const auto &event : m_events) {
        if (event.type == TimelineEvent::StateChange && !isCallEventRow(event))
            ++count;
    }
    return count;
}

int TimelineModel::stateGroupCount() const
{
    // Count groups by counting the rows that lead one; stateGroupLeaderRow owns
    // the grouping rule, so this cannot drift from what is drawn.
    int groups = 0;
    for (int row = 0; row < m_events.size(); ++row) {
        if (m_events.at(row).type != TimelineEvent::StateChange)
            continue;
        if (isCallEventRow(m_events.at(row)))
            continue;
        if (stateGroupLeaderRow(row) == row)
            ++groups;
    }
    return groups;
}

QVariantList TimelineModel::stateGroupEntriesFrom(int leaderRow) const
{
    QVariantList entries;
    for (int i = leaderRow; i < m_events.size();) {
        const auto &e = m_events.at(i);
        // A call ends the run rather than being skipped: it draws its own row,
        // so the state rows on either side are separate annotations.
        if (isCallEventRow(e))
            break;
        if (e.type == TimelineEvent::StateChange) {
            // Call membership is skipped but does not end the run: the rows
            // keep their positions (groups are defined by adjacency) and simply
            // produce no entries. A membership-only group yields an empty list,
            // which draws nothing.
            if (isRtcMembershipKind(e.stateKind)) {
                ++i;
                continue;
            }
            QVariantMap entry;
            entry.insert(QStringLiteral("stableEventId"),
                         e.itemId.isEmpty() ? e.eventId : e.itemId);
            entry.insert(QStringLiteral("eventId"), e.eventId);
            entry.insert(QStringLiteral("eventKind"), e.stateKind);
            // Closed set for the row's glyph; never parsed from the translated
            // sentence.
            entry.insert(QStringLiteral("membershipChange"),
                         e.membershipChange);
            entry.insert(QStringLiteral("actorUserId"), e.sender);
            entry.insert(QStringLiteral("actorDisplayName"),
                         senderDisplayName(e));
            entry.insert(QStringLiteral("affectedMemberDisplayName"), e.stateTarget);
            entry.insert(QStringLiteral("description"), visibleBodyFor(e));
            entry.insert(QStringLiteral("timestamp"), e.timestamp);
            entries.append(entry);
            ++i;
            continue;
        }
        // Mirror of stateGroupLeaderRow: a date divider ends the run.
        if (e.type == TimelineEvent::DateDivider)
            break;
        if (e.isVirtual()) {
            ++i;
            continue;
        }
        break;
    }
    return entries;
}

bool TimelineModel::rowHostsReceipts(int row) const
{
    if (row < 0 || row >= m_events.size())
        return false;
    const auto &e = m_events.at(row);
    if (e.isVirtual() || e.type == TimelineEvent::DateDivider)
        return false;
    // A call card is content with a body of its own.
    if (isCallEventRow(e))
        return true;
    if (e.type != TimelineEvent::StateChange)
        return true;
    // Call-membership updates never draw.
    if (isRtcMembershipKind(e.stateKind))
        return false;
    // A state run draws one summary, on its leader, and only when at least one
    // entry survives the activity settings.
    if (stateGroupLeaderRow(row) != row)
        return false;
    const QVariantList entries = stateGroupEntriesFrom(row);
    for (const auto &entry : entries) {
        const QString kind =
            entry.toMap().value(QStringLiteral("eventKind")).toString();
        if (activityKindVisible(kind))
            return true;
    }
    return false;
}

int TimelineModel::receiptHostRow(int row) const
{
    if (row < 0 || row >= m_events.size())
        return -1;
    for (int r = row; r >= 0; --r) {
        if (rowHostsReceipts(r))
            return r;
    }
    return -1;
}

const QHash<QString, int> &TimelineModel::latestReceiptRows() const
{
    if (m_latestReceiptRowDirty) {
        m_latestReceiptRow.clear();
        for (int r = 0; r < m_events.size(); ++r) {
            for (const auto &receipt : m_events.at(r).readBy)
                m_latestReceiptRow.insert(receipt.userId, r); // ascending: last wins
        }
        m_latestReceiptRowDirty = false;
    }
    return m_latestReceiptRow;
}

QList<ReadReceipt> TimelineModel::hostedReceipts(int row,
                                                 int *reportedTotal) const
{
    // One entry per reader, their newest receipt winning. Walk forward while
    // rows are hosted here; the first self-hosting row ends the span.
    QHash<QString, ReadReceipt> byUser;
    int spanEnd = row;
    // Readers beyond a row's delivered window cannot be deduplicated, so the
    // total is the unique delivered readers plus each row's undelivered
    // remainder, never a sum of per-row totals.
    int undelivered = 0;
    for (int r = row; r < m_events.size(); ++r) {
        if (r != row && rowHostsReceipts(r))
            break;
        spanEnd = r;
        const auto &e = m_events.at(r);
        undelivered += qMax(0, e.readByTotal - static_cast<int>(e.readBy.size()));
        for (const auto &receipt : e.readBy) {
            const auto it = byUser.constFind(receipt.userId);
            if (it == byUser.constEnd() || it->tsMs < receipt.tsMs)
                byUser.insert(receipt.userId, receipt);
        }
    }
    // One position per reader: a reader carried by a newer row than this span
    // is shown there (the backend may briefly leave an older receipt behind).
    const QHash<QString, int> &latest = latestReceiptRows();
    for (auto it = byUser.begin(); it != byUser.end();) {
        if (latest.value(it.key(), -1) > spanEnd)
            it = byUser.erase(it);
        else
            ++it;
    }
    if (reportedTotal)
        *reportedTotal = static_cast<int>(byUser.size()) + undelivered;
    QList<ReadReceipt> out = byUser.values();
    std::stable_sort(out.begin(), out.end(),
                     [](const ReadReceipt &a, const ReadReceipt &b) {
                         return a.tsMs > b.tsMs;
                     });
    return out;
}

QHash<QString, QString> TimelineModel::receiptPositionsByEvent() const
{
    QHash<QString, QString> out;
    const QHash<QString, int> &latest = latestReceiptRows();
    for (auto it = latest.constBegin(); it != latest.constEnd(); ++it) {
        if (it.value() >= 0 && it.value() < m_events.size())
            out.insert(it.key(), m_events.at(it.value()).eventId);
    }
    return out;
}

void TimelineModel::announceReceiptHost(int row, const QHash<QString, QString> &before,
                                        bool hostingChanged, bool announceRow)
{
    // A moved receipt leaves one host and reaches another; both must be told or
    // the old host keeps drawing the reader. The caller captures `before` by
    // event id ahead of its mutation, since the cached index is invalidated and
    // rows may have shifted.
    //
    // Announce only what changed: `row` itself only when asked, and the
    // neighbour above only when a self-hosting row appeared, vanished or
    // flipped.
    invalidateReceiptIndex();
    const QHash<QString, int> &after = latestReceiptRows();
    QSet<int> hosts;
    for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
        const int oldRow = rowForEventId(it.value());
        const int newRow = after.value(it.key(), -1);
        if (oldRow >= 0 && oldRow == newRow)
            continue;
        if (oldRow >= 0)
            hosts.insert(receiptHostRow(oldRow));
        if (newRow >= 0)
            hosts.insert(receiptHostRow(newRow));
    }
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        if (!before.contains(it.key()))
            hosts.insert(receiptHostRow(it.value()));
    }
    if (hostingChanged && row > 0)
        hosts.insert(receiptHostRow(row - 1));
    if (announceRow)
        hosts.insert(row);
    else
        hosts.remove(row);
    for (int h : std::as_const(hosts)) {
        if (h < 0 || h >= m_events.size())
            continue;
        const auto idx = index(h);
        Q_EMIT dataChanged(idx, idx, { ReadReceiptsRole, ReadReceiptsTotalRole });
    }
}

QString TimelineModel::profileChangeDescription(const TimelineEvent &e,
                                                const QString &actorDisplayName)
{
    const QString actor = actorDisplayName.isEmpty()
        ? matrix::user_lookup::localpartOrUserId(e.sender)
        : actorDisplayName;
    // Untrusted plain text from another user's profile: substituted into a tr()
    // string and rendered as PlainText, never rich text or markup.
    const QString oldName = e.profileNameOld.trimmed();
    const QString newName = e.profileNameNew.trimmed();
    const bool avatar = e.profileAvatarChanged;

    if (e.profileNameChange == QLatin1String("changed")
        && !oldName.isEmpty() && !newName.isEmpty()) {
        return avatar
            ? tr("%1 changed their display name from \u201C%2\u201D to "
                 "\u201C%3\u201D and changed their avatar.")
                  .arg(actor, oldName, newName)
            : tr("%1 changed their display name from \u201C%2\u201D to "
                 "\u201C%3\u201D.")
                  .arg(actor, oldName, newName);
    }
    if (e.profileNameChange == QLatin1String("set") && !newName.isEmpty()) {
        return avatar
            ? tr("%1 set their display name to \u201C%2\u201D and changed "
                 "their avatar.").arg(actor, newName)
            : tr("%1 set their display name to \u201C%2\u201D.")
                  .arg(actor, newName);
    }
    if (e.profileNameChange == QLatin1String("cleared")) {
        return avatar
            ? tr("%1 cleared their display name and changed their avatar.")
                  .arg(actor)
            : tr("%1 cleared their display name.").arg(actor);
    }
    if (e.profileNameChange.isEmpty() && avatar)
        return tr("%1 changed their avatar.").arg(actor);
    // Nothing typed arrived, or a name change was claimed without the names
    // (legal; the bridge bounds them). Say only what is known; an empty new
    // name is the separate "cleared" case.
    return tr("%1 updated their profile.").arg(actor);
}

QString TimelineModel::callEventDescription(const TimelineEvent &e,
                                           const QString &actorDisplayName)
{
    const QString actor = actorDisplayName.isEmpty()
        ? matrix::user_lookup::localpartOrUserId(e.sender)
        : actorDisplayName;
    // Only the actor, a resolved display name, is substituted. Nothing the
    // caller wrote appears: the row carries a Join control, and remote free
    // text must never label a control. The declined count is a separate role,
    // so no plural lives in this sentence.
    //
    // "started a call" covers video too: a legacy m.call.invite states no
    // intent, so the generic wording is the only honest one when video is not
    // known.
    return callRowIsVideo(e) ? tr("%1 started a video call.").arg(actor)
                             : tr("%1 started a call.").arg(actor);
}

QString TimelineModel::visibleBodyFor(const TimelineEvent &e) const
{
    if (e.redacted)
        return QStringLiteral("[message deleted]");
    // The Rust bridge sends an empty body for typed profile changes, so every
    // reader of `body` must come through here. Backends that phrase the row
    // themselves (mock, HTTP, older cache) keep their own sentence.
    if (e.type == TimelineEvent::StateChange
        && e.stateKind == QLatin1String("member_profile")
        && (!e.profileNameChange.isEmpty() || e.profileAvatarChanged
            || e.body.isEmpty()))
        return profileChangeDescription(e, senderDisplayName(e));
    // As above: the Rust bridge sends call rows with an empty body on purpose;
    // rows another backend phrased keep their sentence.
    if (isCallEventRow(e) && e.body.isEmpty())
        return callEventDescription(e, senderDisplayName(e));
    return e.body;
}

bool TimelineModel::dividerIntroducesVisibleContent(int dividerRow) const
{
    // A date divider is shown only when something it introduces is drawn, by
    // the delegate's own rules (kept here so delegates need not scan
    // neighbours):
    //   * any non-virtual, non-state row draws;
    //   * a state row draws only its group summary, and only on the leader;
    //   * a routine state row draws nothing while room activity is hidden.
    bool leaderChecked = false;
    for (int row = dividerRow + 1; row < m_events.size(); ++row) {
        const auto &e = m_events.at(row);
        if (e.type == TimelineEvent::DateDivider)
            return false;   // the next day answers for its own run
        if (e.isVirtual())
            continue;       // read markers / timeline-start draw no content
        // A call always draws its own row, so a divider introducing one keeps
        // its date even when the rest of the run is hidden.
        if (isCallEventRow(e))
            return true;
        if (e.type != TimelineEvent::StateChange)
            return true;
        const bool routine = !e.stateKind.isEmpty();
        if (routine && !activityKindVisible(e.stateKind))
            continue;
        // A date divider ends a state run, so the first drawable state row
        // after it leads its own group. Still derived from stateGroupLeaderRow,
        // and only the first drawable state row needs asking; the rest share
        // its run.
        if (!leaderChecked) {
            leaderChecked = true;
            if (stateGroupLeaderRow(row) == row)
                return true;
        }
    }
    return false;
}

// Which routine state rows are shown. rust/src/timeline.rs emits state_kind
// "membership" and "member_profile"; anything else (settings, topic, name…)
// follows the master switch alone.
bool TimelineModel::activityKindVisible(const QString &stateKind) const
{
    if (!m_showRoomActivity)
        return false;
    if (stateKind == QLatin1String("membership"))
        return m_showMembershipEvents;
    if (stateKind == QLatin1String("member_profile"))
        return m_showProfileChangeEvents;
    return true;
}

void TimelineModel::setShowRoomActivity(bool show)
{
    if (m_showRoomActivity == show)
        return;
    m_showRoomActivity = show;
    Q_EMIT showRoomActivityChanged();
    refreshActivityPresentation();
}

void TimelineModel::setShowMembershipEvents(bool show)
{
    if (m_showMembershipEvents == show)
        return;
    m_showMembershipEvents = show;
    Q_EMIT showMembershipEventsChanged();
    refreshActivityPresentation();
}

void TimelineModel::setShowProfileChangeEvents(bool show)
{
    if (m_showProfileChangeEvents == show)
        return;
    m_showProfileChangeEvents = show;
    Q_EMIT showProfileChangeEventsChanged();
    refreshActivityPresentation();
}

void TimelineModel::refreshActivityPresentation()
{
    // Every divider depends on these preferences, so re-announce the loaded
    // range through the existing presentation refresh. Runs once per toggle and
    // is shared by all three setters.
    const int exposed = rowCount();
    if (exposed > 0)
        emitPresentationGroupingChanged(0, exposed - 1);
}

void TimelineModel::emitPresentationGroupingChanged(int first, int last)
{
    const int exposed = rowCount();
    if (exposed == 0)
        return;
    first = qBound(0, first, exposed - 1);
    last = qBound(first, last, exposed - 1);

    // State-activity groups are transparent through virtual rows and can cross
    // the insertion boundary; expand only across that run, which visible rows
    // terminate. Redacted rows group too, so redacting one changes the leader
    // and count of the whole run.
    const auto groupingRunRow = [this](int row) {
        const auto &event = m_events.at(row);
        return event.type == TimelineEvent::StateChange || event.isVirtual()
            || event.redacted;
    };
    while (first > 0 && groupingRunRow(first))
        --first;
    while (last + 1 < exposed && groupingRunRow(last))
        ++last;

    Q_EMIT dataChanged(index(first), index(last),
                       { StateGroupIdRole, StateGroupLeaderRole,
                         StateGroupEntriesRole,
                         DeletedGroupLeaderRole, DeletedGroupCountRole,
                         SameSenderAsPreviousRole,
                         BeginsSenderGroupRole, ContinuesSenderGroupRole,
                         EndsSenderGroupRole, ShowSenderIdentityRole,
                         // A divider depends on the run it introduces, so the
                         // same expansion refreshes it.
                         DividerIntroducesVisibleContentRole });
}

void TimelineModel::rebuildThreadReplyIndex()
{
    // One pass per structural mutation (a pagination batch is one mutation).
    // Only true m.thread replies carry a threadRootId, so this is usually
    // small.
    m_threadReplyCounts.clear();
    for (const auto &e : m_events) {
        if (!e.threadRootId.isEmpty())
            ++m_threadReplyCounts[e.threadRootId];
    }
}

QVariant TimelineModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0)
        return {};
    const int raw = index.row();
    if (raw >= m_events.size())
        return {};
    const auto &e = m_events.at(raw);
    switch (role) {
    case EventIdRole:            return e.eventId;
    case SenderRole:             return e.sender;
    case SenderDisplayNameRole: return senderDisplayName(e);
    case BodyRole:               return visibleBodyFor(e);
    case FormattedBodyRole: {
        // Untrusted sender HTML: never expose it to QML raw. Sanitize to the
        // safe RichText subset and rewrite mentions to resolved display names.
        if (e.redacted || e.formattedBody.isEmpty())
            return QString();
        // The sanitize walk is expensive and this role is re-read on member
        // hydration, so it is memoized per event id; invalidated on edit,
        // replace, redact and theme-colour change, and wholesale on hydration
        // and reload.
        const auto memo = m_sanitizedHtmlCache.constFind(e.eventId);
        if (memo != m_sanitizedHtmlCache.constEnd()) {
            // Resolved on every read, never stored resolved: the edit path
            // reads the memo back, and a local image:// source baked in would
            // be sent to the room.
            return MessageHtml::resolveInlineImages(memo.value(),
                                                    m_inlineImageResolver);
        }
        QList<QPair<QString, QString>> deps;
        QString sanitized = MessageHtml::sanitize(
            e.formattedBody,
            [this, &deps](const QString &userId) {
                const QString name = mentionNameFor(userId, /*ask=*/true);
                deps.append({userId, name});
                return name;
            },
            m_selfUserId,
            MessageHtml::MentionStyle{m_mentionAccentColor,
                                      m_mentionLinkColor,
                                      m_codeBackgroundColor},
            m_spoilersRevealed.contains(e.eventId));
        if (!e.eventId.isEmpty()) {
            m_sanitizedHtmlCache.insert(e.eventId, sanitized);
            if (deps.isEmpty())
                m_htmlMemberDeps.remove(e.eventId);
            else
                m_htmlMemberDeps.insert(e.eventId, deps);
        }
        if (sanitized.contains(QLatin1String("data-mx-emoticon")))
            m_hasInlineEmoji = true;
        return MessageHtml::resolveInlineImages(sanitized,
                                                m_inlineImageResolver);
    }
    case MessageSegmentsRole: {
        if (e.redacted || e.formattedBody.isEmpty())
            return QVariantList{};
        // Fast path: a body without <pre> cannot produce a code-block segment
        // (the same test MessageHtml::segments starts with), so ordinary
        // messages cost one substring scan and nothing is cached.
        if (!e.formattedBody.contains(QLatin1String("<pre"),
                                      Qt::CaseInsensitive))
            return QVariantList{};
        const auto memo = m_messageSegmentsCache.constFind(e.eventId);
        if (memo != m_messageSegmentsCache.constEnd())
            return memo.value();
        const QString roomId = m_realRoomId;
        MatrixClient *client = m_client;
        const QList<MessageHtml::Segment> parsed = MessageHtml::segments(
            e.formattedBody,
            [client, roomId](const QString &userId) {
                return client ? client->displayNameFor(roomId, userId)
                              : QString();
            },
            m_selfUserId,
            MessageHtml::MentionStyle{m_mentionAccentColor,
                                      m_mentionLinkColor,
                                      m_codeBackgroundColor},
            m_spoilersRevealed.contains(e.eventId));
        QVariantList out;
        bool hasCodeBlock = false;
        for (const auto &segment : parsed) {
            if (segment.kind == MessageHtml::SegmentKind::CodeBlock) {
                hasCodeBlock = true;
                break;
            }
        }
        // A body that passed the substring test but yields no code block
        // answers empty, so QML keeps the single-TextEdit path.
        if (hasCodeBlock) {
            out.reserve(parsed.size());
            for (const auto &segment : parsed) {
                QVariantMap m;
                m.insert(QStringLiteral("kind"),
                         static_cast<int>(segment.kind));
                m.insert(QStringLiteral("text"), segment.text);
                m.insert(QStringLiteral("language"), segment.language);
                out.append(m);
            }
        }
        if (!e.eventId.isEmpty())
            m_messageSegmentsCache.insert(e.eventId, out);
        return out;
    }
    case TimestampRole:          return e.timestamp;
    case TypeRole:               return static_cast<int>(e.type);
    case StatusRole:             return static_cast<int>(e.status);
    case IsOwnRole:              return e.sender == m_selfUserId;
    case EditedRole:             return e.edited;
    case RedactedRole:           return e.redacted;
    case ReplyToEventIdRole:     return e.replyToEventId;
    // The SDK embeds the replied-to sender as a raw MXID; resolve it like every
    // other visible identity: member lookup, then the localpart.
    case ReplyToSenderRole: {
        // The Rust backend already resolved the name from the embedded event's
        // profile and sends the MXID separately. Prefer that: the member lookup
        // only knows people this client has seen in the room.
        if (!e.replyToSenderId.isEmpty()) {
            if (!e.replyToSender.isEmpty()
                && e.replyToSender != e.replyToSenderId)
                return e.replyToSender;
            if (m_client) {
                const QString display = m_client->displayNameFor(
                    memberLookupRoomId(e.roomId), e.replyToSenderId);
                if (!display.isEmpty() && display != e.replyToSenderId)
                    return display;
            }
            return matrix::user_lookup::localpartOrUserId(e.replyToSenderId);
        }
        // Older backends put the MXID in replyToSender and carry no profile.
        if (e.replyToSender.isEmpty())
            return QString();
        if (m_client) {
            const QString display = m_client->displayNameFor(
                memberLookupRoomId(e.roomId), e.replyToSender);
            // Backends return the raw user id when nothing is known:
            // unresolved, not a display name.
            if (!display.isEmpty() && display != e.replyToSender)
                return display;
        }
        return matrix::user_lookup::localpartOrUserId(e.replyToSender);
    }
    // The raw MXID behind the quote, used only for the identity colour hash.
    case ReplyToSenderIdRole:
        return e.replyToSenderId.isEmpty()
                   ? (e.replyToSender.contains(QLatin1Char(':'))
                          ? e.replyToSender
                          : QString())
                   : e.replyToSenderId;
    case ReplyToPreviewRole:     return e.replyToPreview;
    case ReplyToMediaKeyRole:    return e.replyToMediaKey;
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
    case IsVideoRole:            return e.type == TimelineEvent::Video;
    case IsAudioRole:            return e.type == TimelineEvent::Audio;
    case IsStickerRole:          return e.type == TimelineEvent::Sticker;
    case MediaDurationMsRole:    return static_cast<qint64>(e.mediaDurationMs);
    case MediaIsVoiceRole:       return e.mediaIsVoice;
    case MediaWaveformRole: {
        QVariantList out;
        out.reserve(e.mediaWaveform.size());
        for (int amp : e.mediaWaveform)
            out.append(amp / 100.0); // QML consumes normalized 0..1
        return out;
    }
    case ReactionsRole:          return reactionsVariant(e);
    case ReadReceiptsRole: {
        // A bodiless row presents no readers of its own; they show on the row
        // hosting it (see rowHostsReceipts).
        if (!rowHostsReceipts(index.row()))
            return QVariantList{};
        TimelineEvent merged = e;
        merged.readBy = hostedReceipts(index.row(), nullptr);
        return readReceiptsVariant(merged);
    }
    case ReadReceiptsTotalRole: {
        // Other readers for the "+N" chip: the uncapped server count minus self
        // when found in the delivered window. A self-receipt beyond the capped
        // window cannot be detected, so it may overcount by one in rooms with
        // more than 16 readers, never undercount.
        if (!rowHostsReceipts(index.row()))
            return 0;
        int reported = 0;
        const QList<ReadReceipt> all = hostedReceipts(index.row(), &reported);
        int excluded = 0;
        for (const auto &r : all) {
            if (r.userId == m_selfUserId)
                ++excluded;
        }
        return qMax(0, qMax(reported, static_cast<int>(all.size())) - excluded);
    }
    case IsPollRole:             return e.type == TimelineEvent::Poll;
    case PollQuestionRole:       return e.pollQuestion;
    case PollKindRole:           return e.pollKind;
    case PollMaxSelectionsRole:  return e.pollMaxSelections;
    case PollAnswersRole:        return pollAnswersVariant(e);
    case PollTotalVotersRole:    return e.pollTotalVoters;
    case PollEndedRole:          return e.pollEnded;
    case IsLocationRole:         return e.type == TimelineEvent::Location;
    case LocationHasPointRole:   return e.locationHasPoint;
    case LocationLatRole:        return e.locationLat;
    case LocationLonRole:        return e.locationLon;
    case LocationUncertaintyRole: return e.locationUncertaintyM;
    case LocationDescriptionRole: return e.locationDescription;
    case LocationAssetRole:      return e.locationAsset;
    case LocationLiveRole:       return e.locationLive;
    case LocationLiveActiveRole: return e.locationLiveActive;
    // Conservative, mirroring canRedactEvent: End poll is offered only on the
    // user's own running polls; the server and receivers enforce MSC3381.
    case CanEndPollRole:
        return e.type == TimelineEvent::Poll && !e.pollEnded
            && e.sender == m_selfUserId;
    case ThreadRootIdRole:       return e.threadRootId;
    case IsThreadRootRole: {
        // The SDK's bundled thread summary is authoritative when present;
        // otherwise use the loaded-reply index (mock/HTTP, or an SDK row
        // without a summary yet). O(1).
        if (e.isThreadRoot)
            return true;
        return m_threadReplyCounts.contains(e.eventId);
    }
    case ThreadReplyCountRole: {
        if (e.threadReplyCount >= 0)
            return e.threadReplyCount;   // SDK summary (server aggregation)
        return m_threadReplyCounts.value(e.eventId, 0);
    }
    case ThreadLatestPreviewRole:   return e.threadLatestPreview;
    case ThreadLatestKindRole:      return e.threadLatestKind;
    case ThreadLatestSenderRole:    return e.threadLatestSender;
    // Same resolution as senderDisplayName(): embedded SDK name, member lookup,
    // then the localpart.
    case ThreadLatestSenderDisplayNameRole: {
        if (!e.threadLatestSenderDisplayName.isEmpty())
            return e.threadLatestSenderDisplayName;
        if (e.threadLatestSender.isEmpty())
            return QString();
        if (m_client) {
            const QString display = m_client->displayNameFor(
                memberLookupRoomId(e.roomId), e.threadLatestSender);
            if (!display.isEmpty() && display != e.threadLatestSender)
                return display;
        }
        return matrix::user_lookup::localpartOrUserId(e.threadLatestSender);
    }
    case ThreadLatestSenderAvatarMxcRole:
        return e.threadLatestSenderAvatarUrl;
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
    case UploadProgressRole:
        // -1 = uploading, extent unknown. Only a known total yields a fraction;
        // clamped because the SDK may revise the combined file+thumbnail total.
        return e.uploadTotalBytes > 0
            ? qBound(0.0, static_cast<double>(e.uploadedBytes)
                              / static_cast<double>(e.uploadTotalBytes), 1.0)
            : -1.0;
    case IsVirtualRole:          return e.isVirtual();
    // Meaningful on a date divider; true elsewhere so QML can read it on every
    // row without a type test.
    case DividerIntroducesVisibleContentRole:
        return e.type != TimelineEvent::DateDivider
            || dividerIntroducesVisibleContent(raw);
    case MediaKeyRole:           return e.mediaKey;
    case MediaSourceAvailableRole: return e.mediaSourceAvailable;
    case MediaThumbAvailableRole:  return e.mediaThumbAvailable;
    case SenderNameAmbiguousRole:  return e.senderNameAmbiguous;
    case SameSenderAsPreviousRole:
    case ContinuesSenderGroupRole: return continuesSenderGroup(raw);
    case BeginsSenderGroupRole:
    case ShowSenderIdentityRole:
        return isVisualMessage(e) && !continuesSenderGroup(raw);
    case EndsSenderGroupRole: {
        if (!isVisualMessage(e))
            return false;
        const int next = nextMessageRowForGrouping(raw);
        return next < 0 || !continuesSenderGroup(next);
    }
    case SenderAvatarMxcRole: {
        if (!e.senderAvatarUrl.isEmpty())
            return e.senderAvatarUrl;
        return m_client ? m_client->avatarMxcFor(memberLookupRoomId(e.roomId),
                                                 e.sender)
                        : QString{};
    }
    case SenderInitialsRole: return senderInitials(e);
    case StableEventIdRole: return e.itemId.isEmpty() ? e.eventId : e.itemId;
    // A call row answers false to both, in either shape: it is not part of a
    // collapsed group and is never hidden by the room-activity preference.
    case IsStateActivityRole:
        return e.type == TimelineEvent::StateChange && !isCallEventRow(e);
    // Typed membership/profile/room-state events are routine. StateChange rows
    // without a kind (untyped notifications) are not proven routine and stay
    // visible.
    case IsRoutineActivityRole:
        return e.type == TimelineEvent::StateChange && !e.stateKind.isEmpty()
            && !isCallEventRow(e);
    case IsCallEventRole: return isCallEventRow(e);
    case CallEventTextRole:
        return isCallEventRow(e) ? visibleBodyFor(e) : QString{};
    case CallIsVideoRole: return isCallEventRow(e) && callRowIsVideo(e);
    case CallDeclinedCountRole:
        return isCallEventRow(e) ? e.callDeclinedCount : 0;
    case GalleryItemsRole: {
        QVariantList out;
        if (e.redacted)
            return out;
        out.reserve(e.galleryItems.size());
        for (const GalleryItem &g : e.galleryItems)
            out.append(galleryItemVariant(g));
        return out;
    }
    case ReplyToKindRole:        return e.redacted ? QString{} : e.replyToKind;
    case ReplyToCountRole:       return e.redacted ? 0 : e.replyToCount;
    case StateKindRole: return e.stateKind;
    case StateGroupIdRole: {
        const int leader = stateGroupLeaderRow(raw);
        if (leader < 0) return QString{};
        const auto &first = m_events.at(leader);
        return first.itemId.isEmpty() ? first.eventId : first.itemId;
    }
    case StateGroupLeaderRole: {
        const int leader = stateGroupLeaderRow(raw);
        return leader == raw;
    }
    case StateGroupEntriesRole: {
        const int leader = stateGroupLeaderRow(raw);
        if (leader != raw) return QVariantList{};
        return stateGroupEntriesFrom(leader);
    }
    case DeletedGroupLeaderRole: {
        const int leader = deletedGroupLeaderRow(raw);
        // Non-redacted rows report true so a delegate can bind
        // `visible: deletedGroupLeader` without also testing `redacted`.
        return leader < 0 || leader == raw;
    }
    case DeletedGroupCountRole: {
        const int leader = deletedGroupLeaderRow(raw);
        if (leader != raw) return 0;
        return deletedGroupLengthFrom(leader);
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
        { FormattedBodyRole,       "formattedBody" },
        { TimestampRole,           "timestamp" },
        { TypeRole,                "eventType" },
        { StatusRole,              "status" },
        { IsOwnRole,               "isOwn" },
        { EditedRole,              "edited" },
        { RedactedRole,            "redacted" },
        { ReplyToEventIdRole,      "replyToEventId" },
        { ReplyToSenderRole,       "replyToSender" },
        { ReplyToSenderIdRole,     "replyToSenderId" },
        { ReplyToPreviewRole,      "replyToPreview" },
        { ReplyToMediaKeyRole,      "replyToMediaKey" },
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
        { ThreadLatestKindRole,      "threadLatestKind" },
        { ThreadLatestSenderRole,    "threadLatestSender" },
        { ThreadLatestSenderDisplayNameRole, "threadLatestSenderDisplayName" },
        { ThreadLatestSenderAvatarMxcRole,   "threadLatestSenderAvatarMxc" },
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
        { UploadProgressRole,      "uploadProgress" },
        { IsVirtualRole,           "isVirtual" },
        { MediaKeyRole,            "mediaKey" },
        { MediaSourceAvailableRole, "mediaSourceAvailable" },
        { MediaThumbAvailableRole, "mediaThumbAvailable" },
        { SenderNameAmbiguousRole, "senderNameAmbiguous" },
        { SameSenderAsPreviousRole, "sameSenderAsPrevious" },
        { IsStateActivityRole,      "isStateActivity" },
        { IsRoutineActivityRole,    "isRoutineActivity" },
        { IsCallEventRole,          "isCallEvent" },
        { CallEventTextRole,        "callEventText" },
        { CallIsVideoRole,          "callIsVideo" },
        { CallDeclinedCountRole,    "callDeclinedCount" },
        { StateKindRole,            "stateKind" },
        { StateGroupIdRole,         "stateGroupId" },
        { StateGroupLeaderRole,     "stateGroupLeader" },
        { StateGroupEntriesRole,    "stateGroupEntries" },
        { DeletedGroupLeaderRole,   "deletedGroupLeader" },
        { DeletedGroupCountRole,    "deletedGroupCount" },
        { SenderAvatarMxcRole,      "senderAvatarMxc" },
        { SenderInitialsRole,       "senderInitials" },
        { BeginsSenderGroupRole,    "beginsSenderGroup" },
        { ContinuesSenderGroupRole, "continuesSenderGroup" },
        { EndsSenderGroupRole,      "endsSenderGroup" },
        { ShowSenderIdentityRole,   "showSenderIdentity" },
        { StableEventIdRole,        "stableEventId" },
        { IsVideoRole,              "isVideo" },
        { IsAudioRole,              "isAudio" },
        { IsStickerRole,            "isSticker" },
        { MediaDurationMsRole,      "mediaDurationMs" },
        { MediaIsVoiceRole,         "mediaIsVoice" },
        { MediaWaveformRole,        "mediaWaveform" },
        { IsPollRole,               "isPoll" },
        { PollQuestionRole,         "pollQuestion" },
        { PollKindRole,             "pollKind" },
        { PollMaxSelectionsRole,    "pollMaxSelections" },
        { PollAnswersRole,          "pollAnswers" },
        { PollTotalVotersRole,      "pollTotalVoters" },
        { PollEndedRole,            "pollEnded" },
        { IsLocationRole,           "isLocation" },
        { LocationHasPointRole,     "locationHasPoint" },
        { LocationLatRole,          "locationLat" },
        { LocationLonRole,          "locationLon" },
        { LocationUncertaintyRole,  "locationUncertaintyM" },
        { LocationDescriptionRole,  "locationDescription" },
        { LocationAssetRole,        "locationAsset" },
        { LocationLiveRole,         "locationLive" },
        { LocationLiveActiveRole,   "locationLiveActive" },
        { CanEndPollRole,           "canEndPoll" },
        { ReadReceiptsRole,         "readReceipts" },
        { ReadReceiptsTotalRole,    "readReceiptsTotal" },
        { MessageSegmentsRole,      "messageSegments" },
        { DividerIntroducesVisibleContentRole,
                                    "dividerIntroducesVisibleContent" },
        { GalleryItemsRole,         "galleryItems" },
        { ReplyToKindRole,          "replyToKind" },
        { ReplyToCountRole,         "replyToCount" },
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
    for (int raw = 0; raw < m_events.size(); ++raw) {
        const TimelineEvent &e = m_events.at(raw);
        const bool isMedia = e.type == TimelineEvent::Image
            || e.type == TimelineEvent::File
            || e.type == TimelineEvent::Video
            || e.type == TimelineEvent::Audio
            || e.type == TimelineEvent::Sticker;
        if (!isMedia || e.redacted)
            continue;
        // Usable when the media bridge can fetch it (Rust) or an HTTP download
        // URL exists.
        const QUrl httpUrl = mediaHttp(e.mediaMxcUrl);
        if (!e.mediaSourceAvailable && httpUrl.isEmpty())
            continue;
        // A gallery contributes one entry per attachment, in the sender's
        // order, so the viewer pages through all of them and on into the rest
        // of the room.
        if (!e.galleryItems.isEmpty()) {
            for (const GalleryItem &g : e.galleryItems) {
                const bool image = g.kind == QLatin1String("image");
                const bool video = g.kind == QLatin1String("video");
                QVariantMap entry;
                entry.insert(QStringLiteral("row"), raw);
                entry.insert(QStringLiteral("mediaKey"), g.mediaKey);
                entry.insert(QStringLiteral("filename"), g.filename);
                entry.insert(QStringLiteral("sender"), senderDisplayName(e));
                entry.insert(QStringLiteral("timestamp"), e.timestamp);
                entry.insert(QStringLiteral("mime"), g.mimetype);
                entry.insert(QStringLiteral("httpUrl"), QUrl{});
                entry.insert(QStringLiteral("isImage"), image);
                entry.insert(QStringLiteral("isVideo"), video);
                entry.insert(QStringLiteral("isVisual"), image || video);
                entry.insert(QStringLiteral("thumbAvailable"), g.thumbAvailable);
                entry.insert(QStringLiteral("size"), g.size);
                out.append(entry);
            }
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("row"), raw);
        entry.insert(QStringLiteral("mediaKey"), e.mediaKey);
        entry.insert(QStringLiteral("filename"), e.mediaFilename);
        entry.insert(QStringLiteral("sender"), senderDisplayName(e));
        entry.insert(QStringLiteral("timestamp"), e.timestamp);
        entry.insert(QStringLiteral("mime"), e.mediaMimetype);
        entry.insert(QStringLiteral("httpUrl"), httpUrl);
        // Stickers navigate through the image viewer like images.
        entry.insert(QStringLiteral("isImage"),
                     e.type == TimelineEvent::Image
                         || e.type == TimelineEvent::Sticker);
        entry.insert(QStringLiteral("isVideo"),
                     e.type == TimelineEvent::Video);
        entry.insert(QStringLiteral("isVisual"),
                     e.type == TimelineEvent::Image
                         || e.type == TimelineEvent::Video
                         || e.type == TimelineEvent::Sticker);
        entry.insert(QStringLiteral("thumbAvailable"),
                     e.mediaThumbAvailable);
        entry.insert(QStringLiteral("size"), static_cast<qint64>(e.mediaSize));
        out.append(entry);
    }
    return out;
}

// The accent, not a colour of its own: the accent ink is spent on mentions
// that concern the reader, and @room concerns every reader. Element paints a
// red pill, but Qt's rich-text engine supports neither border-radius nor
// padding on inline runs (see MessageHtml.h), and red ink alone reads as an
// error.
QString TimelineModel::markRoomMention(const QString &safeHtml) const
{
    return MessageHtml::markRoomMention(safeHtml, m_mentionAccentColor);
}

void TimelineModel::setMentionStyle(const QString &accentColor,
                                    const QString &softColor,
                                    const QString &codeBackground,
                                    const QString &linkColor)
{
    // Only opaque hex colour literals may enter the sanitizer's style
    // attribute:
    //   * defence in depth against style break-out; Qt paints an unparseable
    //     value as solid black rather than dropping it;
    //   * Qt reads eight digits as #aarrggbb (not CSS #rrggbbaa), and a
    //     translucent ink composites against a backdrop the sanitizer cannot
    //     know, so the rendered colour is unpredictable. Reject and fall back.
    static const QRegularExpression hexColor(
        QStringLiteral("^#[0-9a-fA-F]{6}$"));
    // softColor is ignored (the mention chip no longer has a surface; see
    // MessageHtml::MentionStyle); the parameter keeps the QML call's arity.
    Q_UNUSED(softColor);
    QString nextAccent, nextLink, nextCode;
    // Validated independently so one bad value does not disable all mention
    // styling.
    if (hexColor.match(accentColor).hasMatch())
        nextAccent = accentColor.toLower();
    if (hexColor.match(linkColor).hasMatch())
        nextLink = linkColor.toLower();
    if (hexColor.match(codeBackground).hasMatch())
        nextCode = codeBackground.toLower();
    if (nextAccent == m_mentionAccentColor && nextLink == m_mentionLinkColor
        && nextCode == m_codeBackgroundColor)
        return;
    m_mentionAccentColor = nextAccent;
    m_mentionLinkColor = nextLink;
    m_codeBackgroundColor = nextCode;
    clearRenderedHtml();
    const int exposed = rowCount();
    if (exposed > 0)
        Q_EMIT dataChanged(index(0), index(exposed - 1),
                           {FormattedBodyRole, MessageSegmentsRole});
}

const QHash<QString, int> &TimelineModel::rowIndex() const
{
    if (m_rowIndexDirty) {
        m_rowIndex.clear();
        m_rowIndex.reserve(m_events.size());
        for (int i = 0; i < m_events.size(); ++i) {
            const QString &id = m_events.at(i).eventId;
            // First wins on a duplicate id, matching the batch index in
            // RustSdkMatrixClient.
            if (!id.isEmpty() && !m_rowIndex.contains(id))
                m_rowIndex.insert(id, i);
        }
        m_rowIndexDirty = false;
    }
    return m_rowIndex;
}

int TimelineModel::rowForEventId(const QString &eventId) const
{
    if (eventId.isEmpty())
        return -1;
    return rowIndex().value(eventId, -1);
}

void TimelineModel::onEventAppended(const QString &roomId, const TimelineEvent &event)
{
    if (roomId != m_roomId)
        return;
    const QHash<QString, QString> receiptsBefore = receiptPositionsByEvent();
    const int publicRow = static_cast<int>(m_events.size());
    beginInsertRows({}, publicRow, publicRow);
    m_events.append(event);
    noteSenderAvatar(event);
    invalidateRowIndex();
    invalidateReceiptIndex();
    // Incremental: an append adds at most one reply to one root.
    if (!event.threadRootId.isEmpty())
        ++m_threadReplyCounts[event.threadRootId];
    endInsertRows();
    // A bodiless appended row hands its readers to the row above, and a reader
    // it carries leaves its previous host. An append splits no span, so the
    // neighbour is not announced.
    announceReceiptHost(publicRow, receiptsBefore, /*hostingChanged=*/false,
                        /*announceRow=*/false);
    Q_EMIT countChanged();
    emitPresentationGroupingChanged(publicRow - 1, publicRow);
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
    const QHash<QString, QString> receiptsBefore = receiptPositionsByEvent();
    const bool groupingChanged = groupingInputsDiffer(m_events.at(row), newEvent);
    const bool hostedBefore = rowHostsReceipts(row);
    m_events[row] = newEvent;
    noteSenderAvatar(newEvent);
    invalidateRowIndex(); // replacement can rename local: -> remote id
    forgetRenderedHtml(oldEventId);
    forgetRenderedHtml(newEvent.eventId);
    rebuildThreadReplyIndex();
    const auto idx = index(row);
    Q_EMIT dataChanged(idx, idx);
    announceReceiptHost(row, receiptsBefore, hostedBefore != rowHostsReceipts(row),
                        /*announceRow=*/false);
    if (groupingChanged)
        emitPresentationGroupingChanged(row - 1, row + 1);
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
    const int row = rowForEventId(eventId);
    if (row < 0) return;
    // Pull fresh event data from the client's mirror. It applies the same diff
    // stream, so the same row is the O(1) fast path; the scan is a fallback for
    // transient misalignment.
    const auto latest = m_client->timeline(m_roomId);
    const TimelineEvent *fresh = nullptr;
    if (row < latest.size() && latest.at(row).eventId == eventId) {
        fresh = &latest.at(row);
    } else {
        for (const auto &e : latest) {
            if (e.eventId == eventId) {
                fresh = &e;
                break;
            }
        }
    }
    if (!fresh)
        return;
    m_events[row] = *fresh;
    invalidateRowIndex();
    invalidateReceiptIndex();
    forgetRenderedHtml(eventId);
    rebuildThreadReplyIndex();
    const auto idx = index(row);
    Q_EMIT dataChanged(idx, idx, { BodyRole, FormattedBodyRole,
                                   MessageSegmentsRole, EditedRole });
}

void TimelineModel::onEventRedacted(const QString &roomId, const QString &eventId)
{
    if (roomId != m_roomId) return;
    const int row = rowForEventId(eventId);
    if (row < 0) return;
    m_events[row].redacted = true;
    m_events[row].body.clear();
    forgetRenderedHtml(eventId);
    const auto idx = index(row);
    Q_EMIT dataChanged(idx, idx, { BodyRole, RedactedRole, ReactionsRole });
    emitPresentationGroupingChanged(row - 1, row + 1);
}

void TimelineModel::onReactionsChanged(const QString &roomId, const QString &eventId)
{
    if (roomId != m_roomId) return;
    if (!m_client) return;
    const int row = rowForEventId(eventId);
    if (row < 0) return;
    // Same positional fast path as onEventEdited.
    const auto latest = m_client->timeline(m_roomId);
    const TimelineEvent *fresh = nullptr;
    if (row < latest.size() && latest.at(row).eventId == eventId) {
        fresh = &latest.at(row);
    } else {
        for (const auto &e : latest) {
            if (e.eventId == eventId) {
                fresh = &e;
                break;
            }
        }
    }
    if (fresh) {
        m_events[row].reactions = fresh->reactions;
        const auto idx = index(row);
        Q_EMIT dataChanged(idx, idx, { ReactionsRole });
        return;
    }
}

void TimelineModel::onEventsPrepended(const QString &roomId,
                                       const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId) return;
    if (events.isEmpty()) return;
    beginInsertRows({}, 0, events.size() - 1);
    for (int i = events.size() - 1; i >= 0; --i) {
        m_events.prepend(events.at(i));
        noteSenderAvatar(events.at(i));
    }
    invalidateRowIndex();
    invalidateReceiptIndex();
    rebuildThreadReplyIndex();
    endInsertRows();
    Q_EMIT countChanged();
    emitPresentationGroupingChanged(0, events.size());
    // A backward-pagination prepend shifts every existing row by `count`. No
    // consumer today: scroll anchoring reacts to beginInsertRows/endInsertRows.
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
        // Never apply a corrupt index; self-heal from the backend copy.
        reload();
        return;
    }
    // A bodiless inserted row hands its readers to the row above, a reader it
    // carries leaves its previous host, and a bodied one splits the span of the
    // host above.
    const QHash<QString, QString> receiptsBefore = receiptPositionsByEvent();
    beginInsertRows({}, index, index);
    m_events.insert(index, event);
    noteSenderAvatar(event);
    invalidateRowIndex();
    invalidateReceiptIndex();
    if (!event.threadRootId.isEmpty())
        ++m_threadReplyCounts[event.threadRootId];
    endInsertRows();
    announceReceiptHost(index, receiptsBefore, rowHostsReceipts(index),
                        /*announceRow=*/false);
    Q_EMIT countChanged();
    emitPresentationGroupingChanged(index - 1, index + 1);
}

void TimelineModel::onEventsInsertedAt(
    const QString &roomId, int index, const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId || events.isEmpty())
        return;
    if (index < 0 || index > m_events.size()) {
        // Never apply a corrupt range; self-heal from the backend copy.
        reload();
        return;
    }
    const QHash<QString, QString> receiptsBefore = receiptPositionsByEvent();
    beginInsertRows({}, index, index + events.size() - 1);
    for (int offset = 0; offset < events.size(); ++offset) {
        m_events.insert(index + offset, events.at(offset));
        noteSenderAvatar(events.at(offset));
    }
    invalidateRowIndex();
    invalidateReceiptIndex();
    rebuildThreadReplyIndex();
    endInsertRows();
    bool anyHosting = false;
    for (int r = index; r < index + events.size() && !anyHosting; ++r)
        anyHosting = rowHostsReceipts(r);
    announceReceiptHost(index, receiptsBefore, anyHosting, /*announceRow=*/false);
    Q_EMIT countChanged();
    emitPresentationGroupingChanged(index - 1,
                                    index + events.size());
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
    // An in-place SDK Set touches exactly one row: re-read all of that row's
    // roles, never the whole model, or every update relayouts the timeline.
    const QHash<QString, QString> receiptsBefore = receiptPositionsByEvent();
    const bool groupingChanged = groupingInputsDiffer(m_events.at(index), event);
    // The thread-reply index depends only on threadRootId, so a Set that keeps
    // it (receipts, profiles, reactions, send state, decryption) skips the O(n)
    // rebuild.
    const bool threadIndexChanged =
        m_events.at(index).threadRootId != event.threadRootId;
    // `count` cannot change under an in-place Set, but `realCount` can: a Set
    // may turn a virtual row into a real one or back.
    const bool virtualnessChanged =
        m_events.at(index).isVirtual() != event.isVirtual();
    // An in-place Set can turn a row into a call row or stop it being one (late
    // decryption, edit, redaction, re-set cached row) without changing the
    // count, so the latest-call-row cache must be refreshed here or Join stays
    // on the wrong row.
    //
    // onEventReplaced (local -> remote id) is not hooked: it only handles
    // m_pendingSends, and call rows (`m.call.member` state) are never local
    // echoes. Every other mutation path emits countChanged.
    const bool callnessChanged =
        isCallEventRow(m_events.at(index)) != isCallEventRow(event);
    forgetRenderedHtml(m_events.at(index).eventId);
    forgetRenderedHtml(event.eventId);
    const bool hostedBefore = rowHostsReceipts(index);
    m_events[index] = event;
    noteSenderAvatar(event);
    invalidateRowIndex();
    invalidateReceiptIndex();
    if (threadIndexChanged)
        rebuildThreadReplyIndex();
    const auto idx = this->index(index);
    Q_EMIT dataChanged(idx, idx);
    announceReceiptHost(index, receiptsBefore, hostedBefore != rowHostsReceipts(index),
                        /*announceRow=*/false);
    if (groupingChanged)
        emitPresentationGroupingChanged(index - 1, index + 1);
    if (virtualnessChanged)
        Q_EMIT countChanged();
    // Separate from countChanged: callness can change while virtualness does
    // not, and consumers of `count` should not re-read for it.
    if (callnessChanged)
        refreshLatestCallEvent();
}

void TimelineModel::onEventRemovedAt(const QString &roomId, int index)
{
    if (roomId != m_roomId)
        return;
    if (index < 0 || index >= m_events.size()) {
        reload();
        return;
    }
    const QHash<QString, QString> receiptsBefore = receiptPositionsByEvent();
    const bool removedHosted = rowHostsReceipts(index);
    beginRemoveRows({}, index, index);
    const QString removedRoot = m_events.at(index).threadRootId;
    forgetRenderedHtml(m_events.at(index).eventId);
    m_events.removeAt(index);
    invalidateRowIndex();
    invalidateReceiptIndex();
    if (!removedRoot.isEmpty()) {
        const auto it = m_threadReplyCounts.find(removedRoot);
        if (it != m_threadReplyCounts.end() && --it.value() <= 0)
            m_threadReplyCounts.erase(it);
    }
    endRemoveRows();
    Q_EMIT countChanged();
    // The row above may have hosted the removed row's readers.
    announceReceiptHost(qMin(index, static_cast<int>(m_events.size()) - 1),
                        receiptsBefore, removedHosted, /*announceRow=*/true);
    emitPresentationGroupingChanged(index - 1, index);
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
    const int publicSize = static_cast<int>(m_events.size());
    beginRemoveRows({}, length, publicSize - 1);
    while (m_events.size() > length) {
        forgetRenderedHtml(m_events.last().eventId);
        m_events.removeLast();
    }
    invalidateRowIndex();
    invalidateReceiptIndex();
    rebuildThreadReplyIndex();
    endRemoveRows();
    Q_EMIT countChanged();
    emitPresentationGroupingChanged(length - 1, length);
}

void TimelineModel::onLoggedOut()
{
    beginResetModel();
    m_events.clear();
    invalidateRowIndex();
    invalidateReceiptIndex();
    // Rendered message HTML is decrypted plaintext in encrypted rooms; it must
    // not outlive the session.
    clearRenderedHtml();
    m_threadReplyCounts.clear();
    // Session-scoped like every cache here: avatar pairs from account A must
    // not leak into account B's session.
    m_senderAvatarIndex.clear();
    m_roomId.clear();
    m_realRoomId.clear();
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
    // m_realRoomId, not m_roomId: membersChanged names the real room, never a
    // thread's composite id.
    if (roomId != m_realRoomId) return;
    // Precise refresh: mention pills are the only body content that depends on
    // member names, and each cached render records the names it used
    // (m_htmlMemberDeps), so hydration forgets only rows whose answer moved
    // instead of re-rendering every body.
    refreshStaleMentionRows();
    // Identity roles are still announced for every row; they are cheap, and
    // reply headers, receipts, reactions and profile-change rows resolve names
    // through the same lookup and would otherwise keep the localpart fallback.
    const int exposed = rowCount();
    if (exposed > 0) {
        Q_EMIT dataChanged(index(0), index(exposed - 1),
                           { SenderDisplayNameRole, SenderInitialsRole,
                             SenderAvatarMxcRole,
                             ReplyToSenderRole,
                             // Profile-change rows phrase themselves with
                             // the actor's resolved name.
                             BodyRole, StateGroupEntriesRole,
                             ThreadLatestSenderDisplayNameRole,
                             // Receipt chips and reactor names resolve
                             // through the same member lookup.
                             ReadReceiptsRole, ReactionsRole });
    }
    refreshTypingText();
}

int TimelineModel::refreshStaleMentionRows()
{
    QStringList stale;
    for (auto it = m_htmlMemberDeps.constBegin();
         it != m_htmlMemberDeps.constEnd(); ++it) {
        for (const auto &dep : it.value()) {
            if (mentionNameFor(dep.first, /*ask=*/false) != dep.second) {
                stale.append(it.key());
                break;
            }
        }
    }
    if (stale.isEmpty())
        return 0;
    const QHash<QString, int> &rows = rowIndex();
    int announced = 0;
    for (const QString &eventId : std::as_const(stale)) {
        forgetRenderedHtml(eventId);
        const auto row = rows.constFind(eventId);
        if (row == rows.constEnd() || row.value() < 0
            || row.value() >= rowCount())
            continue;
        const QModelIndex idx = index(row.value());
        Q_EMIT dataChanged(idx, idx,
                           { FormattedBodyRole, MessageSegmentsRole });
        ++announced;
    }
    return announced;
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
        // typingUsersFor() keeps m_roomId deliberately: typing is room-wide and
        // onTypingChanged guards against the composite, so the two must agree.
        // The name lookup takes the real room.
        const auto users = m_client->typingUsersFor(m_roomId);
        QStringList names;
        for (const auto &u : users) {
            if (u == m_selfUserId) continue;
            names.append(m_client->displayNameFor(m_realRoomId, u));
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
    // Room timelines only. Reducing a thread's composite id would send an
    // unthreaded m.read naming a thread reply, which is the wrong receipt (a
    // thread needs an MSC3771 threaded receipt via a different SDK call). No
    // caller today; the guard keeps a future one from sending it.
    if (MatrixClient::isThreadTimelineId(m_roomId)) return;
    // The scan is shared with ReadReceiptCoordinator. This path is for explicit
    // user gestures; the automatic policy lives in the coordinator.
    const QString eventId = latestReadableEventId();
    if (!eventId.isEmpty())
        m_client->sendReadReceipt(m_realRoomId, eventId); // deduped downstream
}

QString TimelineModel::latestReadableEventId(qint64 *timestampMs) const
{
    if (timestampMs)
        *timestampMs = 0;
    // Scan back for the newest event with a real remote id. The last row is
    // often virtual (read marker, date divider) or a local echo. Same approach
    // as RoomListModel::markRoomRead.
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
    // The SDK item id survives in-place updates (echo reconciliation, late
    // decryption); prefer it, falling back to the event id.
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

QVariantMap TimelineModel::layoutMetadataAt(int row) const
{
    if (row < 0 || row >= m_events.size())
        return {};

    const auto &e = m_events.at(row);
    QString mediaKind;
    QString rowKind = QStringLiteral("text");
    switch (e.type) {
    case TimelineEvent::Image:
        mediaKind = rowKind = QStringLiteral("image");
        break;
    case TimelineEvent::Video:
        mediaKind = rowKind = QStringLiteral("video");
        break;
    case TimelineEvent::Sticker:
        mediaKind = rowKind = QStringLiteral("sticker");
        break;
    case TimelineEvent::File: rowKind = QStringLiteral("file"); break;
    case TimelineEvent::Audio: rowKind = QStringLiteral("audio"); break;
    case TimelineEvent::Poll: rowKind = QStringLiteral("poll"); break;
    case TimelineEvent::StateChange:
        // Legacy call rows are state events, so ask the predicate rather than
        // the type.
        rowKind = isCallEventRow(e) ? QStringLiteral("call")
                                    : QStringLiteral("state");
        break;
    case TimelineEvent::CallEvent: rowKind = QStringLiteral("call"); break;
    case TimelineEvent::DateDivider:
    case TimelineEvent::ReadMarker:
    case TimelineEvent::TimelineStart:
        rowKind = QStringLiteral("virtual");
        break;
    default: break;
    }

    const QString body = e.body.trimmed();
    const QString filename = e.mediaFilename.trimmed();
    const bool isMediaRow = e.type == TimelineEvent::Image
        || e.type == TimelineEvent::File
        || e.type == TimelineEvent::Video
        || e.type == TimelineEvent::Audio
        || e.type == TimelineEvent::Sticker;
    const bool hasCaption = !e.redacted && isMediaRow
        && !body.isEmpty() && !filename.isEmpty()
        && body.compare(filename, Qt::CaseInsensitive) != 0;

    QVariantMap metadata;
    metadata.insert(QStringLiteral("rowKind"), rowKind);
    metadata.insert(QStringLiteral("mediaKind"), mediaKind);
    metadata.insert(QStringLiteral("mediaWidth"), e.mediaWidth);
    metadata.insert(QStringLiteral("mediaHeight"), e.mediaHeight);
    const QString visibleBody = visibleBodyFor(e);
    metadata.insert(QStringLiteral("bodyLength"), visibleBody.size());
    metadata.insert(QStringLiteral("bodyLineCount"), visibleBody.isEmpty()
                    ? 0 : visibleBody.count(QLatin1Char('\n')) + 1);
    metadata.insert(QStringLiteral("showSenderIdentity"),
                    data(index(row), ShowSenderIdentityRole).toBool());
    metadata.insert(QStringLiteral("isOwn"), e.sender == m_selfUserId);
    metadata.insert(QStringLiteral("hasReply"),
                    !e.redacted && !e.replyToEventId.isEmpty());
    metadata.insert(QStringLiteral("hasCaption"), hasCaption);
    metadata.insert(QStringLiteral("hasMeta"),
                    e.edited || (e.sender == m_selfUserId
                                 && e.status != TimelineEvent::Sent));
    metadata.insert(QStringLiteral("hasThreadSummary"), e.isThreadRoot);
    metadata.insert(QStringLiteral("hasReactions"),
                    !e.redacted && !e.reactions.isEmpty());
    // Uses the same call predicate as IsRoutineActivityRole: this drives the
    // height seed, which must not size a call tile as a collapsed activity
    // line.
    metadata.insert(QStringLiteral("isRoutineActivity"),
                    e.type == TimelineEvent::StateChange
                    && !e.stateKind.isEmpty() && !isCallEventRow(e));
    metadata.insert(QStringLiteral("stateGroupLeader"),
                    e.type == TimelineEvent::StateChange
                    && !isCallEventRow(e)
                    && stateGroupLeaderRow(row) == row);
    metadata.insert(QStringLiteral("pollAnswerCount"), e.pollAnswers.size());
    return metadata;
}

const TimelineEvent *TimelineModel::eventForId(const QString &eventId) const
{
    const int row = rowForEventId(eventId);
    return row >= 0 ? &m_events.at(row) : nullptr;
}

QString TimelineModel::visibleTextForEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->isVirtual() || event->type == TimelineEvent::StateChange
        || event->redacted)
        return {};
    return event->body;
}

QString TimelineModel::mediaKeyForEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->type != TimelineEvent::Image)
        return {};
    return event->mediaKey;
}

void TimelineModel::setInlineImageResolver(
    std::function<QString(const QString &)> resolve)
{
    m_inlineImageResolver = std::move(resolve);
    notifyInlineImagesChanged();
}

void TimelineModel::notifyInlineImagesChanged()
{
    // Only formatted bodies can contain inline emoji, and only repaint if some
    // event has one; media arrives constantly.
    if (!m_hasInlineEmoji || m_events.isEmpty())
        return;
    Q_EMIT dataChanged(index(0), index(int(m_events.size()) - 1),
                       { FormattedBodyRole });
}

QString TimelineModel::sanitizedHtmlForEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->isVirtual() || event->redacted
        || event->formattedBody.isEmpty())
        return {};
    const QString roomId = m_realRoomId;
    MatrixClient *client = m_client;
    return MessageHtml::sanitize(
        event->formattedBody,
        [client, roomId](const QString &userId) {
            return client ? client->displayNameFor(roomId, userId) : QString();
        },
        m_selfUserId,
        MessageHtml::MentionStyle{m_mentionAccentColor, m_mentionLinkColor,
                                  m_codeBackgroundColor},
        // Edit recovery needs the real text: the user is editing their own
        // spoiler.
        /*revealSpoilers=*/true);
}

QString TimelineModel::realRoomIdForEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->roomId.isEmpty())
        return {};
    return MatrixClient::isThreadTimelineId(event->roomId)
               ? MatrixClient::threadTimelineRoomId(event->roomId)
               : event->roomId;
}

QString TimelineModel::messagePermalink(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->roomId.isEmpty() || event->eventId.isEmpty()
        || event->eventId.startsWith(QLatin1String("local:")))
        return {};
    // Thread-timeline events carry the composite id in roomId; a matrix.to link
    // must use the real room id.
    const QString realRoomId =
        MatrixClient::isThreadTimelineId(event->roomId)
            ? MatrixClient::threadTimelineRoomId(event->roomId)
            : event->roomId;
    const auto encodeId = [](const QString &id) {
        return QString::fromLatin1(QUrl::toPercentEncoding(
            id, QByteArrayLiteral("!$:@")));
    };
    return QStringLiteral("https://matrix.to/#/%1/%2")
        .arg(encodeId(realRoomId), encodeId(event->eventId));
}

bool TimelineModel::canRedactEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    return event && event->sender == m_selfUserId && !event->redacted
        && !event->isVirtual() && event->type != TimelineEvent::StateChange
        && !event->eventId.startsWith(QLatin1String("local:"));
}

void TimelineModel::redactEvent(const QString &eventId, const QString &reason)
{
    if (!m_client || m_roomId.isEmpty() || eventId.isEmpty())
        return;
    // m_roomId, not the real room: in a thread this is the composite, which the
    // client decomposes to reach the thread timeline holding the event.
    m_client->redactEvent(m_roomId, eventId, reason);
}

void TimelineModel::toggleReaction(const QString &eventId, const QString &key)
{
    if (!m_client || m_roomId.isEmpty() || eventId.isEmpty() || key.isEmpty())
        return;
    m_client->toggleReaction(m_roomId, eventId, key);
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
    case TimelineEvent::Video:       type = QStringLiteral("m.room.message (video)"); break;
    case TimelineEvent::Audio:       type = QStringLiteral("m.room.message (audio)"); break;
    case TimelineEvent::Sticker:     type = QStringLiteral("m.sticker"); break;
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
    // Show the real room id, not the composite thread-timeline id.
    details.insert(QStringLiteral("roomId"),
                   MatrixClient::isThreadTimelineId(event->roomId)
                       ? MatrixClient::threadTimelineRoomId(event->roomId)
                       : event->roomId);
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

bool TimelineModel::canCancelSend(int row) const
{
    if (!m_client || m_roomId.isEmpty())
        return false;
    if (row < 0 || row >= m_events.size())
        return false;
    const auto &e = m_events.at(row);
    // Only a row with a transaction id can be looked up in the send queue.
    return m_client->supportsCancelSend() && !e.transactionId.isEmpty()
        && (e.status == TimelineEvent::Sending
            || e.status == TimelineEvent::Failed);
}

void TimelineModel::cancelSend(int row)
{
    if (!canCancelSend(row))
        return;
    // Nothing is removed here: the abort can lose a race with the server, and
    // the backend removes the item only when it really aborted.
    m_client->cancelSend(m_roomId, m_events.at(row).transactionId);
}

void TimelineModel::retryDecryption()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    m_client->retryDecryption(m_roomId);
}

void TimelineModel::forgetRenderedHtml(const QString &eventId)
{
    if (eventId.isEmpty())
        return;
    m_sanitizedHtmlCache.remove(eventId);
    m_messageSegmentsCache.remove(eventId);
    m_htmlMemberDeps.remove(eventId);
}

void TimelineModel::toggleSpoilers(const QString &eventId)
{
    // Spoiler reveal state lives in the model, not the delegate: delegates are
    // destroyed when they leave the cache buffer, and the reveal should survive
    // scrolling but die with the timeline.
    if (eventId.isEmpty())
        return;
    if (!m_spoilersRevealed.remove(eventId))
        m_spoilersRevealed.insert(eventId);
    forgetRenderedHtml(eventId);
    for (int row = 0; row < m_events.size(); ++row) {
        if (m_events.at(row).eventId == eventId) {
            const QModelIndex idx = index(row);
            Q_EMIT dataChanged(idx, idx,
                               { FormattedBodyRole, MessageSegmentsRole });
            break;
        }
    }
}

void TimelineModel::clearRenderedHtml()
{
    // Rendered HTML and its segmented form are decrypted plaintext in encrypted
    // rooms; both caches live and die together.
    m_sanitizedHtmlCache.clear();
    m_messageSegmentsCache.clear();
    m_htmlMemberDeps.clear();
}

void TimelineModel::reload()
{
    beginResetModel();
    m_events = (m_client && !m_roomId.isEmpty())
                   ? m_client->timeline(m_roomId)
                   : QList<TimelineEvent>{};
    invalidateRowIndex();
    invalidateReceiptIndex();
    clearRenderedHtml();
    rebuildThreadReplyIndex();
    rebuildSenderAvatarIndex();
    endResetModel();
    Q_EMIT countChanged();
}

void TimelineModel::noteSenderAvatar(const TimelineEvent &event)
{
    // Insert-only for non-empty values: an empty senderAvatarUrl means "not
    // carried on this event", not "avatar removed".
    if (!event.sender.isEmpty() && !event.senderAvatarUrl.isEmpty())
        m_senderAvatarIndex.insert(event.sender, event.senderAvatarUrl);
}

void TimelineModel::rebuildSenderAvatarIndex()
{
    m_senderAvatarIndex.clear();
    for (const auto &event : std::as_const(m_events))
        noteSenderAvatar(event);
}

// ---------------------------------------------------------------------------
// Edit history + event source
// ---------------------------------------------------------------------------

void TimelineModel::requestEditHistory(const QString &eventId)
{
    if (!m_client || eventId.isEmpty())
        return;
    const QString roomId = realRoomIdForEvent(eventId);
    if (roomId.isEmpty())
        return;
    m_client->requestEditHistory(roomId, eventId);
}

void TimelineModel::requestEventSource(const QString &eventId)
{
    if (!m_client || eventId.isEmpty())
        return;
    const QString roomId = realRoomIdForEvent(eventId);
    if (roomId.isEmpty())
        return;
    m_client->requestEventSource(roomId, eventId);
}

QString TimelineModel::sanitizeHtml(const QString &html) const
{
    if (html.isEmpty())
        return {};
    const QString roomId = m_realRoomId;
    MatrixClient *client = m_client;
    return MessageHtml::sanitize(
        html,
        [client, roomId](const QString &userId) {
            return client ? client->displayNameFor(roomId, userId) : QString();
        },
        m_selfUserId,
        MessageHtml::MentionStyle{m_mentionAccentColor, m_mentionLinkColor,
                                  m_codeBackgroundColor},
        /*revealSpoilers=*/true);
}
