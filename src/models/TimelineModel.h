#pragma once

#include "matrix/TimelineEvent.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class TimelineModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(int count READ eventCount NOTIFY countChanged)
    Q_PROPERTY(QString typingText READ typingText NOTIFY typingTextChanged)
    Q_PROPERTY(bool canPaginate READ canPaginate NOTIFY paginationChanged)
    Q_PROPERTY(bool paginating READ paginating NOTIFY paginationChanged)
    // v0.5.7: last backward pagination failed; QML shows a Retry affordance.
    Q_PROPERTY(bool paginationFailed READ paginationFailed NOTIFY paginationChanged)
    // v0.6.1: find-in-loaded-messages. Searches only the events currently
    // present in this timeline (main or thread) — never a server history
    // search, never a persistent plaintext index. State is memory-only and is
    // cleared on endSearch() and on any room/thread switch.
    Q_PROPERTY(bool searchActive READ searchActive NOTIFY searchChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY searchChanged)
    Q_PROPERTY(int searchResultCount READ searchResultCount NOTIFY searchChanged)
    // 1-based position of the current match for display ("2 of 7"); 0 when
    // there are no matches.
    Q_PROPERTY(int searchCurrentPosition READ searchCurrentPosition
                   NOTIFY searchChanged)
    Q_PROPERTY(QString searchCurrentEventId READ searchCurrentEventId
                   NOTIFY searchChanged)
    // v0.7.x: near-top backfill "virtual scrolling" window. While true, a
    // landed backward-pagination prepend is folded into the internal mirror
    // but held out of the exposed row space — see setBackfillStagingActive()
    // in the .cpp for the full mechanism. QML drives this from
    // TimelinePane.qml, bound to (userScrollActive && a near-top run is
    // active), so the ListView sees no structural change while a reader
    // holds a gesture through active loading; flipping back to false flushes
    // the held prefix in one clean insert.
    Q_PROPERTY(bool backfillStagingActive READ backfillStagingActive
                   WRITE setBackfillStagingActive
                   NOTIFY backfillStagingActiveChanged)

public:
    enum Roles {
        EventIdRole = Qt::UserRole + 1,
        SenderRole,
        SenderDisplayNameRole,
        BodyRole,
        FormattedBodyRole, // sanitized Matrix HTML (empty when plaintext)
        TimestampRole,
        TypeRole,
        StatusRole,
        IsOwnRole,
        EditedRole,
        RedactedRole,
        ReplyToEventIdRole,
        ReplyToSenderRole,
        ReplyToPreviewRole,
        MediaMxcUrlRole,
        MediaHttpUrlRole,
        MediaThumbnailHttpUrlRole,
        MediaMimetypeRole,
        MediaFilenameRole,
        MediaSizeRole,
        MediaWidthRole,
        MediaHeightRole,
        IsImageRole,
        IsFileRole,
        ReactionsRole,
        // v0.4.1
        ThreadRootIdRole,        // Non-empty on thread replies.
        IsThreadRootRole,        // True if any event in this room has this as root.
        ThreadReplyCountRole,    // Number of visible replies for a thread root.
        // v0.6.0: SDK thread-summary presentation roles (thread roots only).
        ThreadLatestPreviewRole,   // Sanitized preview of the latest reply.
        ThreadLatestKindRole,      // Semantic kind for a safe label (image/gif/…).
        ThreadLatestSenderRole,    // MXID of the latest reply's sender.
        ThreadLatestSenderDisplayNameRole, // Friendly name (falls back to MXID).
        ThreadLatestSenderAvatarMxcRole,   // mxc:// via the safe avatar path.
        ThreadLatestTimestampRole, // QDateTime of the latest reply.
        ThreadUnreadRole,          // Conservative receipt-based unread hint.
        // v0.6.0 checkpoint 11: m.mentions metadata (SDK-parsed).
        MentionsMeRole,
        MentionsRoomRole,
        // v0.5.0-prep+12: encryption metadata roles surface the four
        // flags C++ already carries on TimelineEvent so MessageDelegate
        // can style undecryptable rows without body-string matching.
        IsEncryptedRole,
        IsDecryptedRole,
        UndecryptableRole,
        ErrorKindRole,
        // v0.5.7: live SDK timeline roles.
        ItemIdRole,          // Stable SDK item id (survives in-place updates).
        IsLocalEchoRole,     // True until the remote echo reconciles.
        SendErrorRole,       // Coarse category when status == Failed.
        IsVirtualRole,       // Date divider / read marker / timeline start.
        // v0.5.9: media bridge + identity presentation.
        MediaKeyRole,             // Retrieval key for MatrixClient::fetchMedia.
        MediaSourceAvailableRole, // Bytes fetchable (incl. encrypted media).
        MediaThumbAvailableRole,  // Server-side thumbnail exists.
        SenderNameAmbiguousRole,  // Display name shared by 2+ members.
        SameSenderAsPreviousRole, // Consecutive-message grouping hint.
        IsStateActivityRole,
        IsRoutineActivityRole,    // Safe for the presentation-only preference.
        StateKindRole,
        StateGroupIdRole,
        StateGroupLeaderRole,
        StateGroupEntriesRole,
        SenderAvatarMxcRole,
        SenderInitialsRole,
        BeginsSenderGroupRole,
        ContinuesSenderGroupRole,
        EndsSenderGroupRole,
        ShowSenderIdentityRole,
        StableEventIdRole,
        // v0.7: typed media presentation (video/audio/voice/sticker rows
        // reserve type-correct geometry before any bytes arrive).
        IsVideoRole,
        IsAudioRole,
        IsStickerRole,
        MediaDurationMsRole,
        MediaIsVoiceRole,
        MediaWaveformRole,   // real MSC3245 envelope (0..100); may be empty
        // v0.7: MSC3381 polls. Aggregation is SDK-owned; these roles only
        // present the outcome. Counts are 0 while an undisclosed poll runs.
        IsPollRole,
        PollQuestionRole,
        PollKindRole,           // "disclosed" | "undisclosed"
        PollMaxSelectionsRole,
        PollAnswersRole,        // list of {id, text, count, byMe}
        PollTotalVotersRole,
        PollEndedRole,
        CanEndPollRole,         // own poll, not ended (conservative rule)
        // Element-style read-receipt chips: OTHER users whose read receipt
        // points at this event, newest first, as a list of
        // {userId, displayName, avatarMxc, tsMs}. Excluded (Element
        // convention): the local user, and the row's SENDER — the SDK
        // synthesizes an implicit receipt for every event's sender, which
        // would otherwise pin a permanent "read by the author" chip to
        // their latest message. Names/avatars resolve live through the
        // same member lookup as every other identity. Thread timelines
        // always answer an empty list: their builders deliberately leave
        // SDK receipt tracking Disabled (the SDK's receipt handling is not
        // thread-aware; enabling it would attach unthreaded receipts to
        // thread rows).
        ReadReceiptsRole,
        // Companion count for the "+N" overflow chip: total OTHER readers
        // (uncapped server-side count minus the exclusions above), >= the
        // list size ReadReceiptsRole answers. The FFI window is capped at
        // 16 newest receipts, so exclusions hiding beyond the window can
        // overcount by at most 2 in >16-reader rooms — conservative, never
        // an undercount of what is visibly shown.
        ReadReceiptsTotalRole,
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    // PUBLIC (exposed) row count — what the view sees. See eventCount()'s
    // sibling internalEventCount() for the true mirror size a policy
    // decision (not a view) should measure against.
    int eventCount() const
    { return static_cast<int>(m_events.size()) - m_hiddenPrefixCount; }
    // The complete mirror size, INCLUDING a currently-staged, not-yet-
    // exposed near-top prefix. PaginationController's progress accounting
    // (batchRowGrowth()) uses this, never eventCount()/rowCount(): whether
    // the reader's near-top run should keep chaining pages is a question
    // about real backend growth, not about what is currently visible.
    int internalEventCount() const { return static_cast<int>(m_events.size()); }

    bool backfillStagingActive() const { return m_backfillStagingActive; }
    void setBackfillStagingActive(bool active);
    // Explicit take-control-of-the-viewport actions (jump-to-event, scroll-
    // anchor restore, reply/restore navigation continuation) must see every
    // row already in the mirror, not just what has been exposed to the
    // ListView so far — rowForStableId()/rowForEventId() answer "not found"
    // for a row still staged, and those callers already treat that as "not
    // loaded yet", which would burn extra backend pages and can even
    // surface the false "Original message is unavailable." for an event
    // that is sitting right there in the mirror. Call this FIRST in any such
    // path. Deliberately does NOT touch backfillStagingActive itself: QML's
    // gesture/run gate keeps owning that decision, so a future prepend can
    // still stage again if the gesture is still in flight when this
    // returns — this only exposes what is CURRENTLY held, once.
    void flushPendingBackfillForNavigation() { flushHiddenPrefix(); }

    QString typingText() const { return m_typingText; }
    bool canPaginate() const;
    bool paginating() const;
    bool paginationFailed() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void requestOlder();
    Q_INVOKABLE void markVisibleAsRead(int firstVisibleRow, int lastVisibleRow);
    Q_INVOKABLE QString ownUserId() const { return m_selfUserId; }

    // v0.5.11: newest event that may legitimately receive a read receipt —
    // skips virtual rows, local echoes without a remote id, and failed
    // sends. Optionally reports the event's origin timestamp so callers can
    // enforce "never regress to an older receipt".
    QString latestReadableEventId(qint64 *timestampMs = nullptr) const;

    // v0.5.11: stable-anchor plumbing for backward-pagination scroll
    // preservation. stableIdAt prefers the SDK item id (survives in-place
    // updates) over the event id; rowForStableId matches either.
    Q_INVOKABLE QString stableIdAt(int row) const;
    // Matrix event id only. Unlike the SDK item id this is suitable for
    // restoring a room after its live timeline has been reconstructed.
    Q_INVOKABLE QString eventIdAt(int row) const;
    Q_INVOKABLE int rowForStableId(const QString &stableId) const;

    // Stable-id message action helpers. Each call re-resolves the event in
    // the current room so a recycled QML delegate cannot act on another row.
    Q_INVOKABLE QString visibleTextForEvent(const QString &eventId) const;
    // The event's sanitized formatted body (same output as
    // FormattedBodyRole), for the edit flow: display-text plain bodies
    // carry no mention markdown, so the composer recovers mention refs
    // from the sanitized mention: anchors instead.
    Q_INVOKABLE QString sanitizedHtmlForEvent(const QString &eventId) const;

    // v0.6.1: loaded-timeline search. beginSearch/updateSearch (re)compute
    // matches over the currently loaded, visible message text; next/prev walk
    // them (wrapping); endSearch clears all state. Case-insensitive.
    bool searchActive() const { return m_searchActive; }
    QString searchQuery() const { return m_searchQuery; }
    int searchResultCount() const { return m_searchResults.size(); }
    int searchCurrentPosition() const
    { return m_searchIndex < 0 ? 0 : m_searchIndex + 1; }
    QString searchCurrentEventId() const
    { return m_searchIndex < 0 ? QString{} : m_searchResults.at(m_searchIndex); }
    Q_INVOKABLE void beginSearch(const QString &query);
    Q_INVOKABLE void updateSearch(const QString &query);
    Q_INVOKABLE void searchNext();
    Q_INVOKABLE void searchPrev();
    Q_INVOKABLE void endSearch();
    Q_INVOKABLE QString messagePermalink(const QString &eventId) const;
    Q_INVOKABLE QVariantMap messageDetails(const QString &eventId) const;
    Q_INVOKABLE bool canEditEvent(const QString &eventId) const;
    Q_INVOKABLE bool canRedactEvent(const QString &eventId) const;

    // v0.5.7: retry a failed outgoing message (row must be a failed local
    // echo with a transaction id). Routed to the backend send queue; the
    // SDK re-attempts the same queued item, so no duplicate can appear.
    Q_INVOKABLE void retrySend(int row);

    // v0.6.0 checkpoint 8: manual decryption retry for this timeline's
    // visible unable-to-decrypt events (backend no-op without a crypto
    // machine).
    Q_INVOKABLE void retryDecryption();

    // v0.5.9: image events currently loaded in this timeline, oldest
    // first, for the image viewer's previous/next navigation. Each entry:
    // {row, mediaKey, filename, sender, timestamp, mime, httpUrl}. Only
    // loaded rows — no history is fetched.
    Q_INVOKABLE QVariantList imageEntries() const;
    // All media events (images + files) currently loaded, oldest first,
    // for the Room Information "Media & Files" list. Adds isImage and
    // size to the imageEntries() shape.
    Q_INVOKABLE QVariantList mediaEntries() const;

    // Theme ink for timeline mention chips (AppTheme.accent/accentSoft),
    // pushed from QML because AppTheme is the sole token source. Values are
    // validated through QColor; anything invalid clears the style and
    // mentions degrade to plain internal links. Re-announces every row's
    // FormattedBodyRole so live theme switches restyle existing rows.
    Q_INVOKABLE void setMentionStyle(const QString &accentColor,
                                     const QString &softColor,
                                     const QString &codeBackground = QString());

Q_SIGNALS:
    void roomIdChanged();
    void countChanged();
    void typingTextChanged();
    void paginationChanged();
    // v0.5.11: a backward-pagination batch prepended `count` rows; existing
    // rows shifted down by exactly that amount. Fired once per EXPOSED
    // insert — a batch held in the backfill staging window (see
    // backfillStagingActive) fires this only when it flushes, as one insert
    // covering everything the run accumulated while held.
    void olderPrepended(int count);
    void searchChanged();
    void backfillStagingActiveChanged();

private Q_SLOTS:
    void onEventAppended(const QString &roomId, const TimelineEvent &event);
    void onEventReplaced(const QString &roomId,
                         const QString &oldEventId,
                         const TimelineEvent &newEvent);
    void onEventStatusChanged(const QString &roomId,
                              const QString &eventId,
                              TimelineEvent::Status status);
    void onEventEdited(const QString &roomId, const QString &eventId);
    void onEventRedacted(const QString &roomId, const QString &eventId);
    void onReactionsChanged(const QString &roomId, const QString &eventId);
    void onEventsPrepended(const QString &roomId, const QList<TimelineEvent> &events);
    void onTimelineReset(const QString &roomId);
    // v0.5.7 index-based diff application. Every index is validated
    // against the local copy; on mismatch the model self-heals by
    // reloading the backend's full timeline instead of corrupting state.
    void onEventInsertedAt(const QString &roomId, int index,
                           const TimelineEvent &event);
    void onEventChangedAt(const QString &roomId, int index,
                          const TimelineEvent &event);
    void onEventRemovedAt(const QString &roomId, int index);
    void onEventsTruncatedTo(const QString &roomId, int length);
    void onLoggedOut();
    void onTypingChanged(const QString &roomId);
    void onMembersChanged(const QString &roomId);
    void onPaginationStateChanged(const QString &roomId);

private:
    // Flush the currently held near-top staging prefix (if any) into the
    // exposed row space in exactly ONE beginInsertRows/endInsertRows. See
    // setBackfillStagingActive() for the full mechanism.
    void flushHiddenPrefix();
    const TimelineEvent *eventForId(const QString &eventId) const;
    // Recompute search matches over the loaded timeline, preserving the
    // currently selected match's event id when it still matches.
    void recomputeSearch();
    void reload();
    int rowForEventId(const QString &eventId) const;
    void refreshTypingText();
    QVariantList reactionsVariant(const TimelineEvent &e) const;
    QVariantList pollAnswersVariant(const TimelineEvent &e) const;
    QVariantList readReceiptsVariant(const TimelineEvent &e) const;
    // Grouping is transparent through virtual rows (date dividers, read
    // markers, timeline-start) — only a visible message/media event ends a
    // group. See TimelineModel.cpp for the rationale.
    int stateGroupLeaderRow(int row) const;
    QVariantList stateGroupEntriesFrom(int leaderRow) const;
    void emitPresentationGroupingChanged();
    // Coalesce presentation-grouping refreshes. Grouping roles are computed
    // live in data(), so the whole-model grouping dataChanged is only a
    // "re-read" notification; a page of N single-item prepend diffs (the SDK
    // delivers backward pagination one push_front at a time) must not fire N
    // whole-model relayouts. scheduleGroupingRefresh() collapses a burst of
    // structural mutations in one event-loop turn into a single emit.
    void scheduleGroupingRefresh();
    QString senderDisplayName(const TimelineEvent &event) const;
    QString senderInitials(const TimelineEvent &event) const;
    bool isVisualMessage(const TimelineEvent &event) const;
    int previousMessageRowForGrouping(int row) const;
    int nextMessageRowForGrouping(int row) const;
    bool continuesSenderGroup(int row) const;
    // True when an in-place event update changed a field the presentation
    // grouping actually reads, so a Set diff only refreshes neighbours when
    // it must (profile/body/media updates never force a grouping sweep).
    bool groupingInputsDiffer(const TimelineEvent &before,
                              const TimelineEvent &after) const;
    QUrl mediaHttp(const QString &mxc) const;
    QUrl mediaThumbHttp(const QString &mxc, int w, int h) const;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    QString m_selfUserId;
    QList<TimelineEvent> m_events;
    // Near-top backfill "virtual scrolling" window. m_events ALWAYS stays
    // the complete, faithful mirror of the backend timeline; these two
    // fields decide how much of its FRONT is currently exposed through
    // QAbstractListModel's row space (rowCount()/data()/index-translating
    // Q_INVOKABLEs). m_hiddenPrefixCount is the invariant: >0 only while
    // m_backfillStagingActive is true, and always driven back to exactly 0
    // by flushHiddenPrefix() or a full reset (reload()/onLoggedOut()) —
    // never left dangling across a room switch. See setBackfillStagingActive
    // in the .cpp for the complete reasoning.
    bool m_backfillStagingActive = false;
    int m_hiddenPrefixCount = 0;
    // Safety cap: if a reader holds a gesture through an unusually long
    // near-top run, force an early flush rather than let the held prefix
    // (and the memory it costs) grow without bound. One extra visible
    // relayout in that rare case is a far better trade than unbounded
    // growth while staged content stays permanently unreachable.
    static constexpr int kMaxHiddenPrefixRows = 400;
    // Loaded thread replies per root event id. IsThreadRootRole and
    // ThreadReplyCountRole used to answer by scanning the WHOLE event list
    // on every query, and every delegate binds both — so each instantiated
    // row cost two full-timeline scans on creation and on every
    // dataChanged, growing linearly with loaded history. That is O(rows x
    // events) per refresh and was a measurable source of scroll jitter
    // while backfilling a long room. Rebuilt on every structural mutation
    // (one pass per batch) so both roles answer in O(1).
    QHash<QString, int> m_threadReplyCounts;
    void rebuildThreadReplyIndex();
    QString m_typingText;

    // Fires once on the next event-loop turn to emit one coalesced
    // whole-model grouping dataChanged. Restarting an already-active
    // single-shot timer collapses a prepend-page burst into one refresh;
    // model resets stop it so no stale refresh chases a cleared/reloaded
    // timeline.
    QTimer m_groupingRefreshTimer;

    // v0.6.1 loaded-timeline search (memory-only; never persisted).
    bool m_searchActive = false;
    QString m_searchQuery;
    QStringList m_searchResults;   // matching event ids, oldest → newest
    int m_searchIndex = -1;        // index into m_searchResults; -1 = none

    // Mention-chip ink (validated #rrggbb/#aarrggbb strings; see
    // setMentionStyle). Empty until QML pushes the current theme.
    QString m_mentionAccentColor;
    QString m_mentionSoftColor;
    QString m_codeBackgroundColor;
};
