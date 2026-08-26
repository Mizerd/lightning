#include "models/TimelineModel.h"

#include "matrix/MatrixClient.h"
#include "models/MessageHtml.h"
#include "models/UserLookup.h"

#include <QRegularExpression>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>

namespace {
// Element/Discord-style visual grouping window. Five minutes is short enough
// to keep conversations scannable while suppressing repetitive identity.
constexpr qint64 kSenderGroupThresholdSeconds = 5 * 60;
}

TimelineModel::TimelineModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Keep an active loaded-timeline search in sync as the timeline changes
    // (pagination prepend, decryption, edits, redactions). Recompute is O(n)
    // over the bounded loaded set and only runs while a search is active.
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
    // Fallback order: room-specific member name carried on the event (SDK
    // sender profile), backend member lookup, then the user id LOCALPART.
    // The complete MXID is never the ordinary visible label — it stays
    // available for tooltips, details, and ambiguity disambiguation.
    if (!event.senderDisplayName.isEmpty())
        return event.senderDisplayName;
    if (m_client) {
        const QString display = m_client->displayNameFor(event.roomId, event.sender);
        // Backends return the raw user id when nothing is known — that is
        // "unresolved", not a display name.
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
/// Whether a call row states VIDEO intent, in either shape.
///
/// The typed field is authoritative; the legacy state-kind spelling
/// ("m.call.video") is the only thing a pre-2026-08-26 row carries, so both
/// are read. False means "not known to be video", NEVER "audio only".
bool callRowIsVideo(const TimelineEvent &e)
{
    return e.callIsVideo || e.stateKind == QLatin1String("m.call.video");
}
} // namespace

// A call somebody started, in EITHER shape.
//
// The Rust bridge emits `msgtype: "call"` (TimelineEvent::CallEvent) since
// 2026-08-26. Before that it emitted a state event with `state_kind
// "m.call"`, and the mock/HTTP backends still can — those rows must render
// as calls too, or a cached timeline (and every test written against the
// mock's state-event helper) falls back into the "N room updates" group this
// exists to get calls out of.
bool TimelineModel::isCallEventRow(const TimelineEvent &e)
{
    if (e.type == TimelineEvent::CallEvent)
        return true;
    return e.type == TimelineEvent::StateChange
        && (e.stateKind == QLatin1String("m.call")
            || e.stateKind == QLatin1String("m.call.video"));
}

bool TimelineModel::isVisualMessage(const TimelineEvent &event) const
{
    // A call row is NOT a visual message for grouping purposes, for the same
    // reason a state row is not: it carries no sender identity header and it
    // must BREAK a sender group rather than continue one. Counting it as a
    // message would let two messages from the same person either side of a
    // call keep one grouped block, hiding the call between two continuation
    // lines.
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
    // The only per-event inputs the sender-grouping and state-grouping
    // computations read. Profile, body, media, reaction, and decryption
    // updates deliberately do not force a neighbourhood grouping refresh.
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

void TimelineModel::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    Q_EMIT roomIdChanged();
    // A room/thread switch clears search state — never carry a query or its
    // plaintext matches across timelines.
    endSearch();
    reload();
    refreshTypingText();
    Q_EMIT paginationChanged();
}

void TimelineModel::beginSearch(const QString &query)
{
    m_searchActive = true;
    m_searchQuery = query;
    recomputeSearch();
    // Start at the newest match (closest to where the user is reading).
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
    // Preserve the currently selected match across a recompute (timeline diff,
    // decryption, edit) when it still matches.
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
            // The event is already in hand — reading its visible text
            // directly (same rules as visibleTextForEvent) avoids the
            // per-row id lookup that made this recompute quadratic.
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
        // Backends return the raw user id when nothing is known — that is
        // "unresolved", not a display name.
        if (!resolved.isEmpty() && resolved != userId)
            return resolved;
    }
    return matrix::user_lookup::localpartOrUserId(userId);
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
        // Who reacted. The bridge delivers a bounded window of stable user
        // ids (16, the read-receipt cap) and resolution happens HERE, at
        // role-read time, through the same member lookup every other
        // identity uses — never a name the bridge guessed, and never a
        // network round trip from a role. `reactorTotal` is the UNCAPPED
        // count, so the tooltip can say "and N more" truthfully instead of
        // inferring an overflow from a list length that was capped.
        QStringList names;
        names.reserve(r.senders.size());
        for (const QString &userId : r.senders) {
            const QString name = memberDisplayName(e.roomId, userId);
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
    // Element convention: ONLY the user's own receipt is hidden. Every
    // other user's marker renders wherever it points — INCLUDING on that
    // user's own message (the SDK's implicit sender receipt): that is
    // precisely how a DM says "they have read up to here", and their chip
    // rides their latest message until they read something newer. The old
    // extra sender-exclusion made receipts vanish asymmetrically the
    // moment someone sent (live two-device report, 2026-08-11: one side
    // showed the read bubble, the other showed nothing). The exclusion
    // lives HERE (one presentation rule for every backend), not in the
    // FFI mirror. Newest readers first so the bounded chip stack and its
    // "+N" overflow always show the most recent readers. Identity
    // resolution mirrors senderDisplayName(): member lookup first, then
    // the LOCALPART — the complete MXID is never the visible label.
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
    QVariantList out;
    out.reserve(receipts.size());
    for (const auto &r : receipts) {
        const QString display = memberDisplayName(e.roomId, r.userId);
        QString avatar =
            m_client ? m_client->avatarMxcFor(e.roomId, r.userId) : QString{};
        // Member-cache miss (hydration pending, failed, or a reader beyond
        // the roster snapshot): fall back to the avatar this timeline has
        // itself seen on the reader's own messages. Without this the chip
        // rendered a letter fallback beside rows showing the same user's
        // picture.
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

// Virtual rows (date dividers, read markers, the timeline-start marker) are
// synthetic SDK bookkeeping items, not visible messages — matrix-sdk-ui
// freely interleaves them between real events (a date divider at every day
// boundary, a read marker wherever the user last read up to). They must not
// split a run of state-change events into separate activity groups; only an
// actual visible message/media event (or the start/end of the timeline)
// ends a group.
namespace {
/// Whether a room-state kind is MatrixRTC call membership.
///
/// Every join, every keepalive refresh and every leave writes one of these
/// state events. They were rendering inside the "N room updates" group as a
/// generic "<user> updated room settings." line each — nine of them for one
/// call in a reported screenshot, none of them telling the reader anything.
/// Element shows a call as a single tile and keeps its membership churn out
/// of the timeline; the call banner and the notification are the equivalents
/// here.
///
/// All four spellings: the legacy session format Element writes today, the
/// sticky MSC4143 format, and the stable names both are heading for. A
/// spelling this misses comes back as spam, so the set is deliberately wide.
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
        // A LEGACY-shaped call row (state event, kind "m.call") is content,
        // not activity: it draws its own tile, so it ends the run exactly
        // like a message does. Without this it would join the run — and, as
        // the run's first row, LEAD it, drawing the group summary on top of
        // its own call tile.
        if (e.type == TimelineEvent::StateChange && !isCallEventRow(e)) {
            leader = probe;
            --probe;
            continue;
        }
        if (e.isVirtual()) {
            --probe;
            continue;
        }
        break; // a visible message/media/call event ends the group here.
    }
    return leader;
}

// Deliberately the same shape as stateGroupLeaderRow, including walking
// THROUGH virtual rows: a date divider between two deletions does not make
// them two separate events to a reader. A run is broken only by a row that is
// actually visible.
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
        if (e.isVirtual())
            continue;
        break;
    }
    return count;
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
    // A group is a maximal run of state rows, so count the runs by counting
    // the rows that lead one. stateGroupLeaderRow already encodes the real
    // grouping rule (including transparency through virtual rows), so this
    // cannot drift from what the timeline actually draws.
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
        // A call ENDS the run rather than being skipped inside it: it draws
        // its own row, so the state rows before and after it are two
        // separate annotations with a piece of room history between them.
        // This is also the whole fix for the reported "1 room update"
        // expanding to "call event".
        if (isCallEventRow(e))
            break;
        if (e.type == TimelineEvent::StateChange) {
            // Call membership is skipped but does NOT end the run: the rows
            // stay in place (they hold their position in the SDK's index
            // space, and the group is defined by adjacency), they simply do
            // not become entries. A group that is nothing BUT membership
            // therefore yields an empty list, which the summary treats as
            // nothing to show.
            if (isRtcMembershipKind(e.stateKind)) {
                ++i;
                continue;
            }
            QVariantMap entry;
            entry.insert(QStringLiteral("stableEventId"),
                         e.itemId.isEmpty() ? e.eventId : e.itemId);
            entry.insert(QStringLiteral("eventId"), e.eventId);
            entry.insert(QStringLiteral("eventKind"), e.stateKind);
            // Closed set, for the row's glyph. Never parsed back out of the
            // sentence — that is translated, and a glyph derived from
            // translated text would be right in one language.
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
        if (e.isVirtual()) {
            ++i;
            continue;
        }
        break;
    }
    return entries;
}

QString TimelineModel::profileChangeDescription(const TimelineEvent &e,
                                                const QString &actorDisplayName)
{
    const QString actor = actorDisplayName.isEmpty()
        ? matrix::user_lookup::localpartOrUserId(e.sender)
        : actorDisplayName;
    // Untrusted plain text, straight from another user's profile. It is
    // substituted into a tr() string and rendered as PlainText — never as
    // rich text, and never concatenated into markup.
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
    // Either nothing typed arrived, or a name change was CLAIMED whose
    // names we do not have (the bridge bounds them, and an old-name-less
    // change is legal). Rendering \u201C\u201D would assert the user set
    // their name to nothing, which is a different event — that is what
    // "cleared" is for. Say only what is supported.
    return tr("%1 updated their profile.").arg(actor);
}

QString TimelineModel::callEventDescription(const TimelineEvent &e,
                                           const QString &actorDisplayName)
{
    const QString actor = actorDisplayName.isEmpty()
        ? matrix::user_lookup::localpartOrUserId(e.sender)
        : actorDisplayName;
    // ONLY the actor is substituted, and it is a resolved display name — the
    // same string every other row shows for this person. Nothing the caller
    // WROTE appears anywhere in this sentence: the row carries a Join
    // control, and free text chosen by a remote sender must never label a
    // control the reader is invited to click (the rule the tombstone banner
    // established). The declined COUNT is a separate role, not part of the
    // sentence, so a translator never has to phrase a plural inside it.
    //
    // "started a call" covers a video call too. A legacy m.call.invite
    // states no intent at all, so `callIsVideo` false means "not known to be
    // video" and the generic wording is the only honest one for it.
    return callRowIsVideo(e) ? tr("%1 started a video call.").arg(actor)
                             : tr("%1 started a call.").arg(actor);
}

QString TimelineModel::visibleBodyFor(const TimelineEvent &e) const
{
    if (e.redacted)
        return QStringLiteral("[message deleted]");
    // The Rust bridge sends an EMPTY body for a typed profile change, so
    // every reader of `body` must come through here or it renders a blank
    // row. Backends that still phrase the row THEMSELVES (mock, HTTP, rows
    // restored from an older cache) keep their own sentence: replacing a
    // body we did not produce with the typed fallback would DISCARD what
    // that backend knew, not translate it.
    if (e.type == TimelineEvent::StateChange
        && e.stateKind == QLatin1String("member_profile")
        && (!e.profileNameChange.isEmpty() || e.profileAvatarChanged
            || e.body.isEmpty()))
        return profileChangeDescription(e, senderDisplayName(e));
    // Same contract as the profile change above, and for the same reason:
    // the Rust bridge sends an EMPTY body for a call row on purpose. A
    // backend that phrased the row itself (mock, HTTP, a row restored from
    // an older cache) keeps its own sentence — replacing a body we did not
    // produce would DISCARD what that backend knew.
    if (isCallEventRow(e) && e.body.isEmpty())
        return callEventDescription(e, senderDisplayName(e));
    return e.body;
}

bool TimelineModel::dividerIntroducesVisibleContent(int dividerRow) const
{
    // A date divider earns its space only when something it introduces is
    // actually drawn. "Drawn" is exactly the delegate's own rule, kept in
    // one place rather than asked of each delegate (which would need to
    // scan its neighbours on every bind):
    //   * any non-virtual, non-state row is a real message/media/poll row;
    //   * a state row draws only its collapsed GROUP SUMMARY, and only the
    //     group LEADER draws that;
    //   * a routine state row draws nothing at all while the room-activity
    //     preference is off.
    bool leaderChecked = false;
    for (int row = dividerRow + 1; row < m_events.size(); ++row) {
        const auto &e = m_events.at(row);
        if (e.type == TimelineEvent::DateDivider)
            return false;   // the next day answers for its own run
        if (e.isVirtual())
            continue;       // read markers / timeline-start draw no content
        // A call draws its own row unconditionally, so a divider that
        // introduces one keeps its date even when every other row in the run
        // is hidden activity.
        if (isCallEventRow(e))
            return true;
        if (e.type != TimelineEvent::StateChange)
            return true;
        const bool routine = !e.stateKind.isEmpty();
        if (routine && !activityKindVisible(e.stateKind))
            continue;
        // State groups are transparent through virtual rows, so a run that
        // began BEFORE this divider keeps its leader up there and this
        // divider introduces no summary of its own — that is precisely the
        // orphan date label this role exists to remove. Only the FIRST
        // drawable state row after the divider can be a leader: everything
        // after it is in the same run (a run can only end at a visible
        // message, which returns true above), so this walk costs one
        // leader resolution, not one per row.
        if (!leaderChecked) {
            leaderChecked = true;
            if (stateGroupLeaderRow(row) == row)
                return true;
        }
    }
    return false;
}

// Which routine state rows are shown. The Rust bridge has distinguished
// these all along — rust/src/timeline.rs emits state_kind "membership" and
// "member_profile" — and only this filter conflated them, by testing that
// the kind was NON-EMPTY rather than testing its value. Anything that is
// neither (room settings, topic, name…) follows the master switch alone,
// because there is no third toggle and inventing one silently would be
// worse than the coarse behaviour it replaced.
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
    // Every divider's answer depends on these preferences, so the whole
    // loaded range is re-announced — through the ONE existing presentation
    // refresh, not a second invalidation path. This runs once per user
    // toggle, never per frame and never per scroll position. Shared by all
    // three setters so a sub-toggle can never refresh differently from the
    // master.
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

    // State-activity groups are transparent through virtual SDK rows and can
    // cross the immediate insertion boundary. Expand only across that run;
    // visible message/media rows terminate it. This remains proportional to
    // the affected group rather than to all loaded history.
    //
    // REDACTED rows join the predicate for the same reason state rows are in
    // it: they group, so redacting one message changes the leader and the
    // count of every other row in its run, and a dataChanged that stopped at
    // the redacted row itself would leave the rest of the run displaying a
    // stale count.
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
                         // A divider's answer is decided by the rows of the
                         // run it introduces, so the same boundary-local
                         // expansion that refreshes the run refreshes it.
                         DividerIntroducesVisibleContentRole });
}

void TimelineModel::rebuildThreadReplyIndex()
{
    // One pass per structural mutation (a whole pagination batch is one
    // mutation), replacing the per-query full scans the thread roles used
    // to do. Only true m.thread replies carry a threadRootId, so this is
    // typically far smaller than the event list.
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
        // Untrusted sender HTML — never expose it to QML raw. Sanitize to the
        // safe RichText subset and rewrite mentions to resolved display names.
        if (e.redacted || e.formattedBody.isEmpty())
            return QString();
        // The full character-walk sanitize is NOT cheap and this role is
        // re-read for every row on member hydration (and twice per binding
        // evaluation before the QML read was deduplicated). Memoized per
        // event id; invalidated on edit/replace/redact/theme-color change,
        // and wholesale on member hydration and reload.
        const auto memo = m_sanitizedHtmlCache.constFind(e.eventId);
        if (memo != m_sanitizedHtmlCache.constEnd())
            return memo.value();
        const QString roomId = m_roomId;
        MatrixClient *client = m_client;
        QString sanitized = MessageHtml::sanitize(
            e.formattedBody,
            [client, roomId](const QString &userId) {
                return client ? client->displayNameFor(roomId, userId)
                              : QString();
            },
            m_selfUserId,
            MessageHtml::MentionStyle{m_mentionAccentColor,
                                      m_mentionLinkColor,
                                      m_codeBackgroundColor});
        if (!e.eventId.isEmpty())
            m_sanitizedHtmlCache.insert(e.eventId, sanitized);
        return sanitized;
    }
    case MessageSegmentsRole: {
        if (e.redacted || e.formattedBody.isEmpty())
            return QVariantList{};
        // The fast path, and the reason this role can exist at all: a body
        // with no <pre> can never produce a code-block segment
        // (MessageHtml::segments' own containsCodeBlock() opens with
        // exactly this test), so the ordinary message is answered by one
        // substring scan — not by a second full sanitize walk beside
        // FormattedBodyRole's. Nothing is cached for it either: the answer
        // is cheaper than the hash lookup that would serve it.
        if (!e.formattedBody.contains(QLatin1String("<pre"),
                                      Qt::CaseInsensitive))
            return QVariantList{};
        const auto memo = m_messageSegmentsCache.constFind(e.eventId);
        if (memo != m_messageSegmentsCache.constEnd())
            return memo.value();
        const QString roomId = m_roomId;
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
                                      m_codeBackgroundColor});
        QVariantList out;
        bool hasCodeBlock = false;
        for (const auto &segment : parsed) {
            if (segment.kind == MessageHtml::SegmentKind::CodeBlock) {
                hasCodeBlock = true;
                break;
            }
        }
        // A body that survived the substring test but yielded no code block
        // (a <pre> inside a dropped element, say) answers EMPTY, so QML
        // keeps its single-TextEdit path. Only a row that really has a code
        // block gets the segmented renderer.
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
    // The SDK embeds the replied-to sender as a raw MXID; resolve it to the
    // room display name like every other visible identity (the reply header
    // used to show the bare localpart — a username — even when the member's
    // name was known). Fallback order mirrors senderDisplayName(): member
    // lookup, then the LOCALPART — the complete MXID is never the label.
    case ReplyToSenderRole: {
        if (e.replyToSender.isEmpty())
            return QString();
        if (m_client) {
            const QString display =
                m_client->displayNameFor(e.roomId, e.replyToSender);
            // Backends return the raw user id when nothing is known — that
            // is "unresolved", not a display name.
            if (!display.isEmpty() && display != e.replyToSender)
                return display;
        }
        return matrix::user_lookup::localpartOrUserId(e.replyToSender);
    }
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
    case ReadReceiptsRole:       return readReceiptsVariant(e);
    case ReadReceiptsTotalRole: {
        // Total OTHER readers for the "+N" chip: the uncapped server-side
        // count minus the single presentation exclusion (self) when found
        // in the delivered window. A self-receipt hiding beyond the capped
        // window cannot be detected — bounded overcount of at most 1, only
        // in >16-reader rooms; never an undercount of what is shown.
        int excluded = 0;
        for (const auto &r : e.readBy) {
            if (r.userId == m_selfUserId)
                ++excluded;
        }
        const int reported =
            qMax(e.readByTotal, static_cast<int>(e.readBy.size()));
        return qMax(0, reported - excluded);
    }
    case IsPollRole:             return e.type == TimelineEvent::Poll;
    case PollQuestionRole:       return e.pollQuestion;
    case PollKindRole:           return e.pollKind;
    case PollMaxSelectionsRole:  return e.pollMaxSelections;
    case PollAnswersRole:        return pollAnswersVariant(e);
    case PollTotalVotersRole:    return e.pollTotalVoters;
    case PollEndedRole:          return e.pollEnded;
    // Conservative permission rule, mirroring canRedactEvent: Lightning
    // offers End poll only on the user's own running polls; the server and
    // receiving clients enforce the actual MSC3381 rules.
    case CanEndPollRole:
        return e.type == TimelineEvent::Poll && !e.pollEnded
            && e.sender == m_selfUserId;
    case ThreadRootIdRole:       return e.threadRootId;
    case IsThreadRootRole: {
        // v0.6.0: the SDK's bundled thread summary is authoritative when
        // present; otherwise consult the loaded-reply index (mock/HTTP
        // backends, and SDK rows whose summary has not arrived). O(1) —
        // this used to scan the whole event list per query, and since the
        // early-out never fires for an ORDINARY row, every delegate paid a
        // full-timeline scan just to be told "not a thread root".
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
    // Same three-tier resolution as senderDisplayName()/ReplyToSenderRole:
    // embedded SDK name, member lookup, then the LOCALPART (review M:
    // the summary card previously skipped the lookup tier and kept a bare
    // username for members whose profile only the roster knows).
    case ThreadLatestSenderDisplayNameRole: {
        if (!e.threadLatestSenderDisplayName.isEmpty())
            return e.threadLatestSenderDisplayName;
        if (e.threadLatestSender.isEmpty())
            return QString();
        if (m_client) {
            const QString display =
                m_client->displayNameFor(e.roomId, e.threadLatestSender);
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
        // -1 = uploading, extent unknown. Only a KNOWN total produces a
        // fraction; clamped because the SDK's combined file+thumbnail total
        // can be revised between reports.
        return e.uploadTotalBytes > 0
            ? qBound(0.0, static_cast<double>(e.uploadedBytes)
                              / static_cast<double>(e.uploadTotalBytes), 1.0)
            : -1.0;
    case IsVirtualRole:          return e.isVirtual();
    // Meaningful on a date divider; true everywhere else so a QML gate can
    // read it on every row without a type test of its own.
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
        return m_client ? m_client->avatarMxcFor(e.roomId, e.sender) : QString{};
    }
    case SenderInitialsRole: return senderInitials(e);
    case StableEventIdRole: return e.itemId.isEmpty() ? e.eventId : e.itemId;
    // A call row answers FALSE to both, in either shape. That is the whole
    // routing decision: `isStateActivity` is what puts a row inside the
    // collapsed group, and `isRoutineActivity` is what lets the
    // room-activity preference hide it. A call is room history — it draws
    // its own tile and it is never hidden by a preference about room
    // settings.
    case IsStateActivityRole:
        return e.type == TimelineEvent::StateChange && !isCallEventRow(e);
    // Typed membership/profile/room-state events are routine annotations.
    // StateChange rows without a kind include untyped notifications and
    // remain visible because they are not proven routine activity.
    case IsRoutineActivityRole:
        return e.type == TimelineEvent::StateChange && !e.stateKind.isEmpty()
            && !isCallEventRow(e);
    case IsCallEventRole: return isCallEventRow(e);
    case CallEventTextRole:
        return isCallEventRow(e) ? visibleBodyFor(e) : QString{};
    case CallIsVideoRole: return isCallEventRow(e) && callRowIsVideo(e);
    case CallDeclinedCountRole:
        return isCallEventRow(e) ? e.callDeclinedCount : 0;
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
        // A non-redacted row reports TRUE so a delegate can bind
        // `visible: deletedGroupLeader` without also testing `redacted`
        // and hiding every ordinary message.
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
        { CanEndPollRole,           "canEndPoll" },
        { ReadReceiptsRole,         "readReceipts" },
        { ReadReceiptsTotalRole,    "readReceiptsTotal" },
        { MessageSegmentsRole,      "messageSegments" },
        { DividerIntroducesVisibleContentRole,
                                    "dividerIntroducesVisibleContent" },
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
        // Usable when the media bridge can fetch it (Rust) or an HTTP
        // download URL exists (HTTP backend).
        const QUrl httpUrl = mediaHttp(e.mediaMxcUrl);
        if (!e.mediaSourceAvailable && httpUrl.isEmpty())
            continue;
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

// The accent, not a colour of its own: this file's two inks are a semantic
// split, and the accent is the one spent on a mention that concerns the
// READER. @room concerns every reader by definition, so it belongs on the
// same ink as a mention of you rather than on the link ink shared with
// mentions of other people and with external URLs.
//
// Element paints a red pill instead. Lightning cannot: Qt's rich-text engine
// honours neither border-radius nor padding on an inline run, and a
// background-color paints an unroundable full-line-height slab that reads as
// a selection highlight (all measured — see MessageHtml.h). Red ink with no
// pill to carry it reads as an error, which is the failure mode that header
// already records.
QString TimelineModel::markRoomMention(const QString &safeHtml) const
{
    return MessageHtml::markRoomMention(safeHtml, m_mentionAccentColor);
}

void TimelineModel::setMentionStyle(const QString &accentColor,
                                    const QString &softColor,
                                    const QString &codeBackground,
                                    const QString &linkColor)
{
    // Only OPAQUE hex color literals may enter the sanitizer's style
    // attribute. Two reasons, both measured against Qt 6.11:
    //   * defense in depth against style break-out (the values normally come
    //     straight from AppTheme, but an unparseable string does NOT make Qt
    //     drop the declaration — it paints a solid BLACK background, so a
    //     sloppy value is a visible defect, not a no-op);
    //   * the eight-digit form is Qt's #aarrggbb, the reverse of CSS Color
    //     4's #rrggbbaa, and it was reaching here from exactly one token
    //     (Storm's `accentSoft`, a 14% bolt). A translucent ink composites
    //     against a backdrop the sanitizer cannot know — the message row may
    //     itself be carrying the mention wash — so the colour that appears is
    //     never the colour the theme chose. Reject it and fall back rather
    //     than render an unpredictable one.
    static const QRegularExpression hexColor(
        QStringLiteral("^#[0-9a-fA-F]{6}$"));
    // softColor is accepted and ignored. It used to be the mention chip's
    // surface; the chip no longer has one (see MessageHtml::MentionStyle),
    // and the parameter stays only so the QML push site keeps its arity.
    Q_UNUSED(softColor);
    QString nextAccent, nextLink, nextCode;
    // Validated independently: they used to share one guard, so a single bad
    // value silently disabled mention styling altogether.
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
            // First-wins on a duplicate id, matching the linear scan this
            // index replaced (and the batch index in RustSdkMatrixClient).
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
    const int publicRow = static_cast<int>(m_events.size());
    beginInsertRows({}, publicRow, publicRow);
    m_events.append(event);
    noteSenderAvatar(event);
    invalidateRowIndex();
    // Incremental: an append can only add one reply to one root — the full
    // O(n) rebuild ran once per live event during sync bursts.
    if (!event.threadRootId.isEmpty())
        ++m_threadReplyCounts[event.threadRootId];
    endInsertRows();
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
    const bool groupingChanged = groupingInputsDiffer(m_events.at(row), newEvent);
    m_events[row] = newEvent;
    noteSenderAvatar(newEvent);
    invalidateRowIndex(); // replacement can rename local: -> remote id
    forgetRenderedHtml(oldEventId);
    forgetRenderedHtml(newEvent.eventId);
    rebuildThreadReplyIndex();
    const auto idx = index(row);
    Q_EMIT dataChanged(idx, idx);
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
    // Pull fresh event data from client cache. The mirror is positionally
    // aligned with this model (both apply the same diff stream), so the
    // same row is the O(1) fast path; the scan remains as a correctness
    // fallback for any transient misalignment.
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
    // Same positional fast path as onEventEdited — the mirror and the model
    // apply the same diff stream, so `row` is almost always the answer.
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
    rebuildThreadReplyIndex();
    endInsertRows();
    Q_EMIT countChanged();
    emitPresentationGroupingChanged(0, events.size());
    // v0.5.11 factual description, corrected: a backward-pagination prepend
    // shifts every existing row down by `count`. This signal has no
    // consumer today — the actual anchor mechanism reacts to the
    // beginInsertRows/endInsertRows pair above via ListView's own
    // onContentHeightChanged, not to this signal.
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
    noteSenderAvatar(event);
    invalidateRowIndex();
    if (!event.threadRootId.isEmpty())
        ++m_threadReplyCounts[event.threadRootId];
    endInsertRows();
    Q_EMIT countChanged();
    emitPresentationGroupingChanged(index - 1, index + 1);
}

void TimelineModel::onEventsInsertedAt(
    const QString &roomId, int index, const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId || events.isEmpty())
        return;
    if (index < 0 || index > m_events.size()) {
        // Never apply a corrupt range — self-heal from the backend copy.
        reload();
        return;
    }
    beginInsertRows({}, index, index + events.size() - 1);
    for (int offset = 0; offset < events.size(); ++offset) {
        m_events.insert(index + offset, events.at(offset));
        noteSenderAvatar(events.at(offset));
    }
    invalidateRowIndex();
    rebuildThreadReplyIndex();
    endInsertRows();
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
    // An in-place SDK Set touches exactly one row. Re-read every role of
    // that row, but never the whole model: the previous full-range
    // dataChanged forced every delegate to re-bind and re-measure on each
    // profile resolution, decryption, reaction, or send-state update, which
    // multiplied one item update into a whole-timeline relayout.
    const bool groupingChanged = groupingInputsDiffer(m_events.at(index), event);
    // The thread-reply index depends ONLY on each row's threadRootId, so a
    // Set that keeps it unchanged (receipt moves, profile resolution,
    // reactions, send-state, decryption) skips the O(n) rebuild — receipts
    // multiplied the Set frequency enough to make the unconditional
    // rebuild a real cost in long timelines.
    const bool threadIndexChanged =
        m_events.at(index).threadRootId != event.threadRootId;
    forgetRenderedHtml(m_events.at(index).eventId);
    forgetRenderedHtml(event.eventId);
    m_events[index] = event;
    noteSenderAvatar(event);
    invalidateRowIndex();
    if (threadIndexChanged)
        rebuildThreadReplyIndex();
    const auto idx = this->index(index);
    Q_EMIT dataChanged(idx, idx);
    if (groupingChanged)
        emitPresentationGroupingChanged(index - 1, index + 1);
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
    const QString removedRoot = m_events.at(index).threadRootId;
    forgetRenderedHtml(m_events.at(index).eventId);
    m_events.removeAt(index);
    invalidateRowIndex();
    if (!removedRoot.isEmpty()) {
        const auto it = m_threadReplyCounts.find(removedRoot);
        if (it != m_threadReplyCounts.end() && --it.value() <= 0)
            m_threadReplyCounts.erase(it);
    }
    endRemoveRows();
    Q_EMIT countChanged();
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
    // Rendered message HTML is decrypted plaintext for encrypted rooms —
    // it must not outlive the session (review M4).
    clearRenderedHtml();
    m_threadReplyCounts.clear();
    // Session-scoped like every cache here: user-id → avatar pairs from
    // account A must not linger into account B's session.
    m_senderAvatarIndex.clear();
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
    clearRenderedHtml(); // mention chips embed resolved names
    // Refresh SDK/member-derived identity for every row (cheap: one signal).
    // FormattedBodyRole and ReplyToSenderRole are member-derived too: mention
    // chips and reply headers resolve display names through the SAME member
    // lookup, and a row rendered before hydration would otherwise keep its
    // localpart fallback (a bare username) forever.
    const int exposed = rowCount();
    if (exposed > 0) {
        Q_EMIT dataChanged(index(0), index(exposed - 1),
                           { SenderDisplayNameRole, SenderInitialsRole,
                             SenderAvatarMxcRole, FormattedBodyRole,
                             MessageSegmentsRole,
                             ReplyToSenderRole,
                             // A typed profile-change row phrases itself
                             // with the ACTOR's resolved name, so hydration
                             // must re-announce the state rows too.
                             BodyRole, StateGroupEntriesRole,
                             ThreadLatestSenderDisplayNameRole,
                             // Receipt chips resolve reader names/avatars
                             // through the same member lookup — hydration must
                             // refresh them off their localpart fallback too,
                             // and so do the reactor names on the chips.
                             ReadReceiptsRole, ReactionsRole });
    }
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
        // The legacy call shape is a state event, so ask the predicate
        // rather than the type — otherwise one call row reports "state" and
        // an identical one reports "call".
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
    // Kept in step with IsRoutineActivityRole / StateGroupLeaderRole by
    // reading the SAME call predicate: this metadata drives the height seed,
    // and a seed that thinks a call row is a collapsed activity line would
    // reserve one text line for a tile.
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

QString TimelineModel::sanitizedHtmlForEvent(const QString &eventId) const
{
    const auto *event = eventForId(eventId);
    if (!event || event->isVirtual() || event->redacted
        || event->formattedBody.isEmpty())
        return {};
    const QString roomId = m_roomId;
    MatrixClient *client = m_client;
    return MessageHtml::sanitize(
        event->formattedBody,
        [client, roomId](const QString &userId) {
            return client ? client->displayNameFor(roomId, userId) : QString();
        },
        m_selfUserId,
        MessageHtml::MentionStyle{m_mentionAccentColor, m_mentionLinkColor,
                                  m_codeBackgroundColor});
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
    // Thread-timeline events carry the composite timeline id in roomId so the
    // model's diff filtering works; a matrix.to link must use the REAL room
    // id, never the internal composite (which embeds a unit separator and the
    // "thread" marker and would produce a broken permalink).
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
    // Show the real room id, not the internal composite thread-timeline id.
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
    // A transaction id is what the send queue can be asked about, so a row
    // without one is not cancellable no matter what it looks like.
    return m_client->supportsCancelSend() && !e.transactionId.isEmpty()
        && (e.status == TimelineEvent::Sending
            || e.status == TimelineEvent::Failed);
}

void TimelineModel::cancelSend(int row)
{
    if (!canCancelSend(row))
        return;
    // Nothing is removed here. The abort can lose a race with the server,
    // and the backend answers by REMOVING the item only when it really
    // aborted — dropping the row locally would hide a message the room has
    // already received.
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
}

void TimelineModel::clearRenderedHtml()
{
    // Rendered message HTML — and its segmented form — is decrypted
    // plaintext in an encrypted room. Both caches live and die together.
    m_sanitizedHtmlCache.clear();
    m_messageSegmentsCache.clear();
}

void TimelineModel::reload()
{
    beginResetModel();
    m_events = (m_client && !m_roomId.isEmpty())
                   ? m_client->timeline(m_roomId)
                   : QList<TimelineEvent>{};
    invalidateRowIndex();
    clearRenderedHtml();
    rebuildThreadReplyIndex();
    rebuildSenderAvatarIndex();
    endResetModel();
    Q_EMIT countChanged();
}

void TimelineModel::noteSenderAvatar(const TimelineEvent &event)
{
    // Deliberately insert-only for non-empty values: an empty
    // senderAvatarUrl means "not carried on this event", not "the user
    // removed their avatar", so it must not erase a known one. A reader
    // who cleared their avatar can keep the old picture on receipt chips
    // until reload — consistent with what their older rows still show.
    if (!event.sender.isEmpty() && !event.senderAvatarUrl.isEmpty())
        m_senderAvatarIndex.insert(event.sender, event.senderAvatarUrl);
}

void TimelineModel::rebuildSenderAvatarIndex()
{
    m_senderAvatarIndex.clear();
    for (const auto &event : std::as_const(m_events))
        noteSenderAvatar(event);
}
