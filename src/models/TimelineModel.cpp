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

    // One coalesced grouping refresh per event-loop turn (see
    // scheduleGroupingRefresh). Interval 0 fires as soon as control returns
    // to the event loop — after the whole poll-drain of prepend diffs has
    // been applied — so a page of older messages relayouts grouping once.
    m_groupingRefreshTimer.setSingleShot(true);
    m_groupingRefreshTimer.setInterval(0);
    connect(&m_groupingRefreshTimer, &QTimer::timeout, this,
            [this] { emitPresentationGroupingChanged(); });
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

bool TimelineModel::isVisualMessage(const TimelineEvent &event) const
{
    return !event.isVirtual() && event.type != TimelineEvent::StateChange;
}

int TimelineModel::previousMessageRowForGrouping(int row) const
{
    // Clamped at m_hiddenPrefixCount, NOT 0: while a near-top run is
    // staging a hidden prefix, the exposed row space must be
    // self-consistent on its own, exactly as if the hidden rows did not
    // exist yet — they do not, from the view's perspective. Without this
    // bound, the topmost EXPOSED row's "previous" lookup could resolve into
    // the hidden prefix and find a same-sender/same-time neighbour there,
    // making continuesSenderGroup() report true and silently drop that
    // row's sender identity header (a ~34px shrink) — mid-gesture content
    // loss from inside the mechanism meant to prevent exactly that class of
    // jitter. flushHiddenPrefix() schedules a grouping refresh, so the true
    // merge (if any) announces once, cleanly, the moment the prefix is
    // exposed — never torn mid-run.
    for (int probe = row - 1; probe >= m_hiddenPrefixCount; --probe) {
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
        // PUBLIC (exposed) range only, starting at m_hiddenPrefixCount: a
        // match still staged inside an active near-top run has no view row
        // to navigate to yet (rowForStableId() reports it unresolvable —
        // see there), so counting it here would show the reader a result
        // they cannot jump to. The flush's beginInsertRows/endInsertRows
        // fires rowsInserted, which the constructor already wires to a
        // resync (see the `resync` lambda), so a staged match is picked up
        // for free the moment it is exposed — no separate hook needed here.
        for (int raw = m_hiddenPrefixCount; raw < m_events.size(); ++raw) {
            const auto &event = m_events.at(raw);
            if (event.isVirtual() || event.eventId.isEmpty())
                continue;
            const QString text = visibleTextForEvent(event.eventId);
            if (text.contains(needle, Qt::CaseInsensitive))
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
    // PUBLIC (exposed) count — see internalEventCount() for the raw mirror
    // size a policy decision should use instead.
    return static_cast<int>(m_events.size()) - m_hiddenPrefixCount;
}

void TimelineModel::setBackfillStagingActive(bool active)
{
    if (m_backfillStagingActive == active)
        return;
    m_backfillStagingActive = active;
    if (!active) {
        // Deactivating IS the flush trigger — QML binds this flag to
        // (userScrollActive && a near-top run is active), so it already
        // turns false at every trigger the design calls for: gesture
        // settle, the run ending (start reached / empty-strike budget
        // spent / no more continuation scheduled), and a room switch
        // (reload() also clears it directly — see there). One clean insert
        // here, through the SAME beginInsertRows/endInsertRows path
        // onEventsPrepended already used per batch, is what lets
        // TimelinePane.qml's existing view-anchor mechanism (idle absolute
        // restore or the mid-gesture relative-delta correction — see
        // maintainViewAnchor()) reconcile the reader's position exactly
        // once instead of once per landed batch.
        flushHiddenPrefix();
    }
    Q_EMIT backfillStagingActiveChanged();
}

void TimelineModel::flushHiddenPrefix()
{
    if (m_hiddenPrefixCount <= 0)
        return;
    const int count = m_hiddenPrefixCount;
    beginInsertRows({}, 0, count - 1);
    m_hiddenPrefixCount = 0;
    endInsertRows();
    Q_EMIT countChanged();
    scheduleGroupingRefresh();
    // No consumer today (see the signal's declaration) — the actual
    // position-preserving mechanism is TimelinePane.qml's
    // maintainViewAnchor(), which reacts to the beginInsertRows/
    // endInsertRows pair above via ListView's own onContentHeightChanged,
    // not to this signal. Emitted anyway for the same reason
    // onEventsPrepended() already does per batch: it is the honest,
    // symmetric description of what just happened (a multi-batch prefix
    // flushed at once is, from any future consumer's point of view,
    // indistinguishable from one big ordinary prepend) and dropping it here
    // while keeping it there would make the two prepend paths inconsistent
    // for no reason.
    Q_EMIT olderPrepended(count);
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
    // Element convention: the user's own receipt is never shown, and
    // neither is the row SENDER's — the SDK synthesizes an implicit
    // receipt for every event's sender, so without this a sender's latest
    // message would permanently carry a "read by its author" chip even
    // when nobody else read it. The exclusions live HERE (one presentation
    // rule for every backend), not in the FFI mirror. Newest readers first
    // so the bounded chip stack and its "+N" overflow always show the most
    // recent readers. Identity resolution mirrors senderDisplayName():
    // member lookup first, then the LOCALPART — the complete MXID is never
    // the visible label.
    QList<ReadReceipt> receipts;
    receipts.reserve(e.readBy.size());
    for (const auto &r : e.readBy) {
        if (r.userId != m_selfUserId && r.userId != e.sender)
            receipts.append(r);
    }
    std::stable_sort(receipts.begin(), receipts.end(),
                     [](const ReadReceipt &a, const ReadReceipt &b) {
                         return a.tsMs > b.tsMs;
                     });
    QVariantList out;
    out.reserve(receipts.size());
    for (const auto &r : receipts) {
        QString display;
        if (m_client) {
            const QString resolved = m_client->displayNameFor(e.roomId, r.userId);
            // Backends return the raw user id when nothing is known — that
            // is "unresolved", not a display name.
            if (!resolved.isEmpty() && resolved != r.userId)
                display = resolved;
        }
        if (display.isEmpty())
            display = matrix::user_lookup::localpartOrUserId(r.userId);
        QVariantMap m;
        m.insert(QStringLiteral("userId"),      r.userId);
        m.insert(QStringLiteral("displayName"), display);
        m.insert(QStringLiteral("avatarMxc"),
                 m_client ? m_client->avatarMxcFor(e.roomId, r.userId)
                          : QString{});
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
int TimelineModel::stateGroupLeaderRow(int row) const
{
    if (row < 0 || row >= m_events.size()
        || m_events.at(row).type != TimelineEvent::StateChange)
        return -1;

    int leader = row;
    int probe = row - 1;
    // Clamped at m_hiddenPrefixCount for the same reason
    // previousMessageRowForGrouping() is: while a near-top run stages a
    // hidden prefix, the leader of an exposed state-change run must never
    // resolve into it. Without this bound a contiguous state-change run
    // that continues into the hidden prefix would resolve EVERY exposed
    // row's leader to a hidden row, so StateGroupLeaderRole is false for
    // all of them and the whole group's presentation (which renders only
    // at the leader) collapses to height 0 mid-gesture.
    while (probe >= m_hiddenPrefixCount) {
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
            entry.insert(QStringLiteral("actorDisplayName"),
                         senderDisplayName(e));
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
    // PUBLIC (exposed) range only — see setMentionStyle()'s identical
    // reasoning. A row still hidden inside an active near-top staging run
    // reads its correct, already-current grouping the moment it is exposed.
    const int exposed = rowCount();
    if (exposed == 0)
        return;
    Q_EMIT dataChanged(index(0), index(exposed - 1),
                       { StateGroupIdRole, StateGroupLeaderRole,
                         StateGroupEntriesRole, SameSenderAsPreviousRole,
                         BeginsSenderGroupRole, ContinuesSenderGroupRole,
                         EndsSenderGroupRole, ShowSenderIdentityRole });
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

void TimelineModel::scheduleGroupingRefresh()
{
    if (m_events.isEmpty())
        return;
    // Restarting the single-shot 0-timer coalesces every mutation in this
    // event-loop turn — the whole 20-diff backward-pagination page, or a
    // burst of live events — into ONE whole-model grouping dataChanged.
    // Grouping is recomputed live in data(), so newly inserted rows already
    // render correctly; existing rows only need this one re-read once the
    // burst has settled. Deferring avoids the per-diff full-model relayout
    // storm that made scrolling jitter while older messages loaded.
    m_groupingRefreshTimer.start();
}

QVariant TimelineModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0)
        return {};
    // v0.7.x: a near-top backfill run stages its landed prefix out of the
    // exposed row space (see setBackfillStagingActive()/flushHiddenPrefix())
    // so the view sees no structural change while a reader holds a gesture
    // through it — this is the "virtual scrolling" translation between the
    // PUBLIC row the view/QML asks for and the RAW position in m_events,
    // which always stays the complete, faithful mirror.
    const int raw = index.row() + m_hiddenPrefixCount;
    if (raw >= m_events.size())
        return {};
    const auto &e = m_events.at(raw);
    switch (role) {
    case EventIdRole:            return e.eventId;
    case SenderRole:             return e.sender;
    case SenderDisplayNameRole: return senderDisplayName(e);
    case BodyRole: {
        if (e.redacted) return QStringLiteral("[message deleted]");
        return e.body;
    }
    case FormattedBodyRole: {
        // Untrusted sender HTML — never expose it to QML raw. Sanitize to the
        // safe RichText subset and rewrite mentions to resolved display names.
        if (e.redacted || e.formattedBody.isEmpty())
            return QString();
        const QString roomId = m_roomId;
        MatrixClient *client = m_client;
        return MessageHtml::sanitize(
            e.formattedBody,
            [client, roomId](const QString &userId) {
                return client ? client->displayNameFor(roomId, userId)
                              : QString();
            },
            m_selfUserId,
            MessageHtml::MentionStyle{m_mentionAccentColor,
                                      m_mentionSoftColor,
                                      m_codeBackgroundColor});
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
        // count minus the presentation exclusions (self, sender) found in
        // the delivered window. Exclusions hiding beyond the capped window
        // cannot be detected — bounded overcount of at most 2, only in
        // >16-reader rooms; never an undercount of what is shown.
        int excluded = 0;
        for (const auto &r : e.readBy) {
            if (r.userId == m_selfUserId || r.userId == e.sender)
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
    case IsVirtualRole:          return e.isVirtual();
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
    case IsStateActivityRole: return e.type == TimelineEvent::StateChange;
    // Typed membership/profile/room-state events are routine annotations.
    // StateChange rows without a kind include call/RTC notifications and
    // remain visible because they are not proven routine activity.
    case IsRoutineActivityRole:
        return e.type == TimelineEvent::StateChange && !e.stateKind.isEmpty();
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
    // Start at m_hiddenPrefixCount: a row still staged inside an active
    // near-top run has no view row yet, so "row" below must stay a valid
    // PUBLIC index the caller can hand back to itemAtIndex()/positionView-
    // AtIndex() — a media item only becomes navigable once its row is
    // exposed at flush, exactly like every other view-facing lookup here.
    for (int raw = m_hiddenPrefixCount; raw < m_events.size(); ++raw) {
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
        entry.insert(QStringLiteral("row"), raw - m_hiddenPrefixCount);
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
        entry.insert(QStringLiteral("size"), static_cast<qint64>(e.mediaSize));
        out.append(entry);
    }
    return out;
}

void TimelineModel::setMentionStyle(const QString &accentColor,
                                    const QString &softColor,
                                    const QString &codeBackground)
{
    // Only hex color literals may enter the sanitizer's style attribute
    // (defense in depth against style break-out; the values normally come
    // straight from AppTheme, whose colors stringify as #rrggbb/#aarrggbb).
    static const QRegularExpression hexColor(
        QStringLiteral("^#(?:[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"));
    QString nextAccent, nextSoft, nextCode;
    if (hexColor.match(accentColor).hasMatch()
        && hexColor.match(softColor).hasMatch()) {
        nextAccent = accentColor.toLower();
        nextSoft = softColor.toLower();
    }
    if (hexColor.match(codeBackground).hasMatch())
        nextCode = codeBackground.toLower();
    if (nextAccent == m_mentionAccentColor && nextSoft == m_mentionSoftColor
        && nextCode == m_codeBackgroundColor)
        return;
    m_mentionAccentColor = nextAccent;
    m_mentionSoftColor = nextSoft;
    m_codeBackgroundColor = nextCode;
    // PUBLIC (exposed) range only: a row still hidden inside an active
    // near-top staging run has no view row to notify yet, and its data()
    // already reads the new mention colors from these member fields once it
    // flushes into view.
    const int exposed = rowCount();
    if (exposed > 0)
        Q_EMIT dataChanged(index(0), index(exposed - 1), {FormattedBodyRole});
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
    // A live/local-echo append at the bottom is NEVER staged — only a
    // near-top backfill prefix is (see setBackfillStagingActive()). The
    // PUBLIC row is the exposed count, not the raw mirror size, whenever a
    // near-top run currently holds a hidden prefix at the front.
    const int publicRow = m_events.size() - m_hiddenPrefixCount;
    beginInsertRows({}, publicRow, publicRow);
    m_events.append(event);
    rebuildThreadReplyIndex();
    endInsertRows();
    Q_EMIT countChanged();
    scheduleGroupingRefresh();
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
    rebuildThreadReplyIndex();
    // A row still staged inside an active near-top run's hidden prefix has
    // no view row to notify — it reads the replaced content the instant it
    // is exposed at flush. See data()'s comment on this translation.
    if (row >= m_hiddenPrefixCount) {
        const auto idx = index(row - m_hiddenPrefixCount);
        Q_EMIT dataChanged(idx, idx);
    }
    if (groupingChanged)
        scheduleGroupingRefresh();
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
    if (row >= m_hiddenPrefixCount) {
        const auto idx = index(row - m_hiddenPrefixCount);
        Q_EMIT dataChanged(idx, idx, { StatusRole });
    }
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
            rebuildThreadReplyIndex();
            if (row >= m_hiddenPrefixCount) {
                const auto idx = index(row - m_hiddenPrefixCount);
                Q_EMIT dataChanged(idx, idx,
                                   { BodyRole, FormattedBodyRole, EditedRole });
            }
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
    if (row >= m_hiddenPrefixCount) {
        const auto idx = index(row - m_hiddenPrefixCount);
        Q_EMIT dataChanged(idx, idx, { BodyRole, RedactedRole, ReactionsRole });
    }
    scheduleGroupingRefresh();
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
            if (row >= m_hiddenPrefixCount) {
                const auto idx = index(row - m_hiddenPrefixCount);
                Q_EMIT dataChanged(idx, idx, { ReactionsRole });
            }
            return;
        }
    }
}

void TimelineModel::onEventsPrepended(const QString &roomId,
                                       const QList<TimelineEvent> &events)
{
    if (roomId != m_roomId) return;
    if (events.isEmpty()) return;
    if (m_backfillStagingActive) {
        // Near-top backfill VIRTUAL SCROLLING window: while a reader holds a
        // gesture through an active near-top run, a landed batch is folded
        // into the mirror but held OUT of the exposed row space instead of
        // being inserted into the view — no beginInsertRows/endInsertRows,
        // so the ListView sees no structural change at all and the reader's
        // viewport is untouched by this batch. It becomes reachable in one
        // clean insert the moment setBackfillStagingActive(false) flushes
        // (gesture settles, the run ends, or the buffer hits its safety
        // cap) — see flushHiddenPrefix(). m_events stays the exact, complete
        // mirror throughout, so every OTHER diff (edit, reaction, decryption,
        // redaction, even a reconciled insert landing inside this prefix)
        // keeps applying correctly against it; only exposure is deferred.
        for (int i = events.size() - 1; i >= 0; --i)
            m_events.prepend(events.at(i));
        // Accepted follow-up, not fixed here: this is a full O(n)
        // rebuildThreadReplyIndex() per staged batch, same as the
        // already-exposed path below. A staged run of many small batches
        // could instead increment m_threadReplyCounts for just the events
        // just prepended (mirroring how appends/removals could too), but
        // that is a performance refinement, not a correctness fix this
        // round is about — left as future work.
        rebuildThreadReplyIndex();
        m_hiddenPrefixCount += static_cast<int>(events.size());
        if (m_hiddenPrefixCount > kMaxHiddenPrefixRows)
            flushHiddenPrefix();
        return;
    }
    beginInsertRows({}, 0, events.size() - 1);
    for (int i = events.size() - 1; i >= 0; --i)
        m_events.prepend(events.at(i));
    rebuildThreadReplyIndex();
    endInsertRows();
    Q_EMIT countChanged();
    scheduleGroupingRefresh();
    // v0.5.11 factual description, corrected: a backward-pagination prepend
    // shifts every existing row down by `count`. This signal has no
    // consumer today — see its declaration and flushHiddenPrefix()'s
    // identical note — the actual anchor mechanism reacts to the
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
    // `index==0` while staging is the single-event equivalent of
    // onEventsPrepended (a bridge that delivers a backward-pagination batch
    // one push_front at a time); `index < m_hiddenPrefixCount` is a
    // reconciled insert landing strictly inside a prefix already staged.
    // m_hiddenPrefixCount is always 0 while staging is inactive, so this
    // condition can only be true here when it should be.
    if (index < m_hiddenPrefixCount
        || (m_backfillStagingActive && index == 0)) {
        m_events.insert(index, event);
        rebuildThreadReplyIndex();
        ++m_hiddenPrefixCount;
        if (m_hiddenPrefixCount > kMaxHiddenPrefixRows)
            flushHiddenPrefix();
        return;
    }
    const int publicRow = index - m_hiddenPrefixCount;
    beginInsertRows({}, publicRow, publicRow);
    m_events.insert(index, event);
    rebuildThreadReplyIndex();
    endInsertRows();
    Q_EMIT countChanged();
    scheduleGroupingRefresh();
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
    m_events[index] = event;
    if (threadIndexChanged)
        rebuildThreadReplyIndex();
    // A row still staged inside the hidden prefix has no view row to notify;
    // the up-to-date data is already in m_events for the moment it flushes.
    if (index >= m_hiddenPrefixCount) {
        const auto idx = this->index(index - m_hiddenPrefixCount);
        Q_EMIT dataChanged(idx, idx);
    }
    if (groupingChanged)
        scheduleGroupingRefresh();
}

void TimelineModel::onEventRemovedAt(const QString &roomId, int index)
{
    if (roomId != m_roomId)
        return;
    if (index < 0 || index >= m_events.size()) {
        reload();
        return;
    }
    if (index < m_hiddenPrefixCount) {
        // Removed strictly inside the still-hidden prefix: the mirror and
        // the hidden count both shrink by one, with no view row to notify.
        m_events.removeAt(index);
        rebuildThreadReplyIndex();
        --m_hiddenPrefixCount;
        return;
    }
    const int publicRow = index - m_hiddenPrefixCount;
    beginRemoveRows({}, publicRow, publicRow);
    m_events.removeAt(index);
    rebuildThreadReplyIndex();
    endRemoveRows();
    Q_EMIT countChanged();
    scheduleGroupingRefresh();
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
    if (length < m_hiddenPrefixCount) {
        // Pathological/very rare: the backend trimmed its cache back further
        // than what this run has staged, eating into the hidden prefix
        // itself. Flush first (exposing everything, m_hiddenPrefixCount ->
        // 0) so the ordinary path below always operates on a plain,
        // consistent public range rather than a second index space.
        flushHiddenPrefix();
    }
    const int publicLength = length - m_hiddenPrefixCount;
    const int publicSize =
        static_cast<int>(m_events.size()) - m_hiddenPrefixCount;
    beginRemoveRows({}, publicLength, publicSize - 1);
    while (m_events.size() > length)
        m_events.removeLast();
    rebuildThreadReplyIndex();
    endRemoveRows();
    Q_EMIT countChanged();
    scheduleGroupingRefresh();
}

void TimelineModel::onLoggedOut()
{
    m_groupingRefreshTimer.stop();
    beginResetModel();
    m_events.clear();
    m_threadReplyCounts.clear();
    m_roomId.clear();
    // See reload()'s identical reasoning: a reset presents everything (here,
    // nothing), so a leftover staged prefix must not survive it.
    m_hiddenPrefixCount = 0;
    if (m_backfillStagingActive) {
        m_backfillStagingActive = false;
        Q_EMIT backfillStagingActiveChanged();
    }
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
    // FormattedBodyRole and ReplyToSenderRole are member-derived too: mention
    // chips and reply headers resolve display names through the SAME member
    // lookup, and a row rendered before hydration would otherwise keep its
    // localpart fallback (a bare username) forever.
    // PUBLIC (exposed) range only — see setMentionStyle()'s identical
    // reasoning. A row still hidden inside an active near-top staging run
    // reads current member data the moment it is exposed.
    const int exposed = rowCount();
    if (exposed > 0) {
        Q_EMIT dataChanged(index(0), index(exposed - 1),
                           { SenderDisplayNameRole, SenderInitialsRole,
                             SenderAvatarMxcRole, FormattedBodyRole,
                             ReplyToSenderRole,
                             ThreadLatestSenderDisplayNameRole,
                             // Receipt chips resolve reader names/avatars
                             // through the same member lookup — hydration must
                             // refresh them off their localpart fallback too.
                             ReadReceiptsRole });
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
    // `row` is PUBLIC (ListView-visible) space; translate to the raw
    // position in the always-complete m_events mirror. See data()'s comment
    // on the same translation.
    if (row < 0)
        return {};
    const int raw = row + m_hiddenPrefixCount;
    if (raw >= m_events.size())
        return {};
    const auto &e = m_events.at(raw);
    // The SDK item id survives in-place updates (local echo reconciliation,
    // late decryption); prefer it and fall back to the event id.
    return e.itemId.isEmpty() ? e.eventId : e.itemId;
}

QString TimelineModel::eventIdAt(int row) const
{
    if (row < 0)
        return {};
    const int raw = row + m_hiddenPrefixCount;
    if (raw >= m_events.size())
        return {};
    return m_events.at(raw).eventId;
}

int TimelineModel::rowForStableId(const QString &stableId) const
{
    if (stableId.isEmpty())
        return -1;
    for (int i = 0; i < m_events.size(); ++i) {
        const auto &e = m_events.at(i);
        if (e.itemId == stableId || e.eventId == stableId) {
            // A match still staged inside the hidden prefix has no view row
            // yet — reported "not found", exactly like a redacted/renamed
            // stable id the caller already treats as genuinely unresolvable
            // (see TimelinePane.qml's maintainViewAnchor()). It becomes
            // findable the instant the run flushes.
            return i < m_hiddenPrefixCount ? -1 : i - m_hiddenPrefixCount;
        }
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
        MessageHtml::MentionStyle{m_mentionAccentColor, m_mentionSoftColor,
                                  m_codeBackgroundColor});
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
    if (row < 0) return;
    const int raw = row + m_hiddenPrefixCount;
    if (raw >= m_events.size()) return;
    const auto &e = m_events.at(raw);
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
    // A full reset re-reads every role at delegate (re)creation, so any
    // pending coalesced grouping refresh for the previous contents is moot;
    // drop it so it cannot fire a spurious whole-model dataChanged against
    // the freshly reloaded rows.
    m_groupingRefreshTimer.stop();
    beginResetModel();
    m_events = (m_client && !m_roomId.isEmpty())
                   ? m_client->timeline(m_roomId)
                   : QList<TimelineEvent>{};
    rebuildThreadReplyIndex();
    // A room/thread switch or any other full reset invalidates a staged
    // near-top prefix outright: the fresh snapshot from the client already
    // reflects the target room in full, and a leftover staged count would
    // silently hide real rows of a room the reader never asked to stage.
    // Set directly (not through the property setter) — a reset already
    // presents everything, so there is nothing to flush, and this must not
    // race the QML gesture/run bindings that reassert the flag afterward.
    m_hiddenPrefixCount = 0;
    if (m_backfillStagingActive) {
        m_backfillStagingActive = false;
        Q_EMIT backfillStagingActiveChanged();
    }
    endResetModel();
    Q_EMIT countChanged();
}
