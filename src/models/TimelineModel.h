#pragma once

#include "matrix/TimelineEvent.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>
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
    // v0.7.4: the presentation preference that decides whether routine
    // room-activity rows render at all. The model needs it because a DATE
    // DIVIDER's own visibility depends on whether anything it introduces is
    // drawn (see DividerIntroducesVisibleContentRole) — a question no
    // delegate can answer for itself without scanning its neighbours. The
    // rows themselves are never filtered here: the timeline stays the
    // authoritative event list and QML keeps the zero-height filter.
    Q_PROPERTY(bool showRoomActivity READ showRoomActivity
                   WRITE setShowRoomActivity NOTIFY showRoomActivityChanged)
    // 2026-08-26: the two halves of "room activity", for the same divider
    // question. Each defaults TRUE so the split is invisible to anyone who
    // never opens it — master on plus both halves on is exactly the previous
    // behaviour — and each is subordinate to the master switch.
    Q_PROPERTY(bool showMembershipEvents READ showMembershipEvents
                   WRITE setShowMembershipEvents
                   NOTIFY showMembershipEventsChanged)
    Q_PROPERTY(bool showProfileChangeEvents READ showProfileChangeEvents
                   WRITE setShowProfileChangeEvents
                   NOTIFY showProfileChangeEventsChanged)

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
        ReplyToSenderIdRole,
        ReplyToPreviewRole,
        ReplyToMediaKeyRole,
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
        // Media-upload progress while status == Sending: 0.0-1.0 when the
        // SDK has reported a total, and -1 when it has NOT. -1 means
        // "uploading, extent unknown" — a text send and a media send whose
        // first progress report has not landed both report it, and the
        // delegate draws an indeterminate bar rather than a 0% one.
        UploadProgressRole,
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
        // Consecutive REDACTED rows collapse the same way a run of state
        // changes does: the first row of a run reports itself the leader and
        // carries the run's length, and the rest render nothing. A moderator
        // clearing twenty messages should cost twenty lines of "[message
        // deleted]" exactly as little as twenty joins cost twenty lines.
        DeletedGroupLeaderRole,
        DeletedGroupCountRole,
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
        // convention): ONLY the local user. A user's marker renders even
        // on their own message (the SDK's implicit sender receipt) — that
        // is how a DM says "they have read up to here"; hiding it made
        // receipts vanish the moment the other side sent (live two-device
        // report, 2026-08-11). Names/avatars resolve live through the
        // same member lookup as every other identity. Thread timelines
        // always answer an empty list: their builders deliberately leave
        // SDK receipt tracking Disabled (the SDK's receipt handling is not
        // thread-aware; enabling it would attach unthreaded receipts to
        // thread rows).
        ReadReceiptsRole,
        // Companion count for the "+N" overflow chip: total OTHER readers
        // (uncapped server-side count minus the self exclusion above), >=
        // the list size ReadReceiptsRole answers. The FFI window is capped
        // at 16 newest receipts, so a self-receipt hiding beyond the
        // window can overcount by at most 1 in >16-reader rooms —
        // conservative, never an undercount of what is visibly shown.
        ReadReceiptsTotalRole,
        // v0.7.4: fenced code blocks. An ordered list of
        // {kind, text, language} maps (kind 0 = rich text, 1 = code block)
        // for a body that actually CONTAINS a code block; EMPTY for every
        // other row, so the ordinary message keeps its single-TextEdit path
        // and its existing cost. See MessageHtml::segments().
        MessageSegmentsRole,
        // v0.7.4: meaningful on DateDivider rows — true when at least one
        // row between this divider and the next one is actually drawn. A
        // divider whose whole run is hidden (routine activity with the
        // preference off, or non-leader rows of a collapsed group) is an
        // orphan date label and must not occupy space. Always true on a
        // non-divider row, so a QML gate can read it unconditionally.
        DividerIntroducesVisibleContentRole,
        // ── 2026-08-26: the typed call row ───────────────────────────────
        // A call somebody started is room HISTORY, so it draws its own row
        // (CallEventDelegate.qml) instead of becoming an entry in the
        // collapsed "N room updates" group — which is what it was, and one
        // call therefore read "1 room update" expanding to the literal words
        // "call event".
        //
        // True on a Rust-backend `call` row AND on the legacy shape (a
        // StateChange whose stateKind is "m.call"/"m.call.video"), so a row
        // that predates the bridge change, or a backend that still phrases
        // calls as state, renders the same way rather than falling back into
        // the activity group.
        IsCallEventRole,
        // The finished sentence, TRANSLATED and written with the actor's
        // resolved display name. The bridge sends an empty body for these
        // rows on purpose: an English sentence built in Rust could be
        // neither translated nor given a resolved name.
        CallEventTextRole,
        // The caller's stated VIDEO intent. False means "not known to be
        // video", never "audio only".
        CallIsVideoRole,
        // How many people declined. A COUNT — the decliners' ids never
        // cross the FFI.
        CallDeclinedCountRole,
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    // Row count — what the view sees. Also the authoritative progress
    // measure for PaginationController's near-top continuation accounting
    // (batchRowGrowth()): with no held/hidden prefix, every row the backend
    // delivers is immediately exposed, so there is no separate "internal
    // mirror size" any more — this IS both the view count and the backend
    // growth count.
    int eventCount() const { return static_cast<int>(m_events.size()); }

    QString typingText() const { return m_typingText; }
    bool canPaginate() const;
    bool paginating() const;
    bool paginationFailed() const;

    bool showRoomActivity() const { return m_showRoomActivity; }
    void setShowRoomActivity(bool show);
    bool showMembershipEvents() const { return m_showMembershipEvents; }
    void setShowMembershipEvents(bool show);
    bool showProfileChangeEvents() const { return m_showProfileChangeEvents; }
    void setShowProfileChangeEvents(bool show);

    // Whether a routine state row of this kind is drawn. PUBLIC because it
    // is a pure query and the sentence-matrix precedent above applies: the
    // filter matrix is worth testing without a backend. The QML zero-height
    // row filter (qml/MessageDelegate.qml, roomActivityVisible) applies the
    // SAME matrix to the rows themselves — keep the two in step.
    bool activityKindVisible(const QString &stateKind) const;

    // Presentation-layer sentence for a typed m.room.member profile change
    // (stateKind == "member_profile"), which the bridge deliberately does
    // NOT phrase: an English sentence built in Rust could be neither
    // translated nor written with the actor's resolved display name.
    // `actorDisplayName` is the resolved name (localpart fallback, never a
    // bare MXID). The old/new names are UNTRUSTED plain text and are
    // rendered as PlainText, never as rich text. Static so the sentence
    // matrix is testable without a model or a backend.
    static QString profileChangeDescription(const TimelineEvent &e,
                                            const QString &actorDisplayName);

    // Whether a row is a call somebody started, in EITHER shape: the Rust
    // backend's typed `call` row, or the legacy "state event with kind
    // m.call" a pre-2026-08-26 bridge (and the mock/HTTP backends) produce.
    // Static and free of model state so the whole routing decision is
    // testable without a backend, and so every reader — grouping, roles,
    // the divider scan — asks exactly one question.
    static bool isCallEventRow(const TimelineEvent &e);
    // Presentation-layer sentence for a call row. Same contract as
    // profileChangeDescription: the bridge sends NO sentence, this builds
    // the translated one with the actor's resolved display name.
    static QString callEventDescription(const TimelineEvent &e,
                                        const QString &actorDisplayName);

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
    // Small presentation-safe subset used by TableView while a row is still
    // unloaded. Reserving media geometry from Matrix `info` metadata avoids
    // the 112px fallback expanding only when an image/video delegate enters
    // the viewport. QML remains the owner of all layout arithmetic.
    Q_INVOKABLE QVariantMap layoutMetadataAt(int row) const;

    // Stable-id message action helpers. Each call re-resolves the event in
    // the current room so a recycled QML delegate cannot act on another row.
    Q_INVOKABLE QString visibleTextForEvent(const QString &eventId) const;
    // Media-bridge key for replying TO an image event (empty otherwise) —
    // lets the composer banner show the same thumbnail the quote will.
    Q_INVOKABLE QString mediaKeyForEvent(const QString &eventId) const;
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
    // v0.7.x: the REAL Matrix room id for an event in this model — thread
    // timelines carry the internal composite id in roomId, which must
    // never reach a protocol call (reporting included).
    Q_INVOKABLE QString realRoomIdForEvent(const QString &eventId) const;
    // Composition of the loaded timeline, for the opt-in scroll trace only
    // (LIGHTNING_SCROLL_TRACE). Counts only — no ids, no bodies, no senders.
    // O(rows), called at most once per completed gesture and only while the
    // trace is on, so it never costs anything in a normal session.
    //
    // These exist so a scroll-performance report can be ANSWERED rather than
    // guessed at: "rows=1200 stateRows=1100 stateGroups=3" and
    // "rows=1200 stateRows=4" are different defects, and the difference is
    // not visible from row count alone.
    Q_INVOKABLE int stateActivityRowCount() const;
    Q_INVOKABLE int stateGroupCount() const;
    Q_INVOKABLE QVariantMap messageDetails(const QString &eventId) const;
    Q_INVOKABLE bool canEditEvent(const QString &eventId) const;
    Q_INVOKABLE bool canRedactEvent(const QString &eventId) const;

    // v0.5.7: retry a failed outgoing message (row must be a failed local
    // echo with a transaction id). Routed to the backend send queue; the
    // SDK re-attempts the same queued item, so no duplicate can appear.
    Q_INVOKABLE void retrySend(int row);
    // Discard a local echo that has not reached the server. Valid while the
    // row is Sending OR Failed: a failed send is still a queued item the
    // user may simply not want any more. The row disappears only when the
    // backend confirms the abort — see the .cpp.
    Q_INVOKABLE bool canCancelSend(int row) const;
    Q_INVOKABLE void cancelSend(int row);

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

    // Theme ink for timeline mentions and links, pushed from QML because
    // AppTheme is the sole token source. accentColor marks a mention of the
    // local user; linkColor covers every other mention and every external
    // URL (empty falls back to accentColor). Values must be OPAQUE #rrggbb —
    // anything else is dropped and that ink degrades to Qt's default link
    // appearance. Re-announces every row's FormattedBodyRole so live theme
    // switches restyle existing rows.
    //
    // softColor is vestigial: it was the chip surface, and the mention no
    // longer has one (MessageHtml::MentionStyle records the measurements).
    // It is kept so the existing QML call site keeps its arity.
    Q_INVOKABLE void setMentionStyle(const QString &accentColor,
                                     const QString &softColor,
                                     const QString &codeBackground = QString(),
                                     const QString &linkColor = QString());

    // Styles the literal "@room" in a body that already carries
    // m.mentions.room (the mentionsRoom role). The delegate calls this for
    // BOTH body paths — sanitized formatted HTML and linkified plain text —
    // because a whole-room mention is plain text in either one.
    //
    // The caller owns the m.mentions.room test. This does not re-check it,
    // and must never be applied to a body that merely contains the words.
    Q_INVOKABLE QString markRoomMention(const QString &safeHtml) const;

Q_SIGNALS:
    void roomIdChanged();
    void countChanged();
    void typingTextChanged();
    void paginationChanged();
    // v0.5.11: a backward-pagination batch prepended `count` rows; existing
    // rows shifted down by exactly that amount. Fired once per landed batch.
    void olderPrepended(int count);
    void searchChanged();
    void showRoomActivityChanged();
    void showMembershipEventsChanged();
    void showProfileChangeEventsChanged();

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
    void onEventsInsertedAt(const QString &roomId, int index,
                            const QList<TimelineEvent> &events);
    void onEventChangedAt(const QString &roomId, int index,
                          const TimelineEvent &event);
    void onEventRemovedAt(const QString &roomId, int index);
    void onEventsTruncatedTo(const QString &roomId, int length);
    void onLoggedOut();
    void onTypingChanged(const QString &roomId);
    void onMembersChanged(const QString &roomId);
    void onPaginationStateChanged(const QString &roomId);

private:
    const TimelineEvent *eventForId(const QString &eventId) const;
    // Recompute search matches over the loaded timeline, preserving the
    // currently selected match's event id when it still matches.
    void recomputeSearch();
    void reload();
    int rowForEventId(const QString &eventId) const;
    // Lazily rebuilt eventId -> row map behind rowForEventId/eventForId.
    // Every structural mutation (and every whole-event replacement, whose
    // id can change local: -> remote) calls invalidateRowIndex(); the next
    // lookup rebuilds once in O(n). Turns the previously-linear lookups —
    // several of which sat inside per-row bindings and per-diff handlers —
    // into amortized O(1).
    void invalidateRowIndex() const { m_rowIndexDirty = true; }
    const QHash<QString, int> &rowIndex() const;
    void refreshTypingText();
    QVariantList reactionsVariant(const TimelineEvent &e) const;
    // One resolver for every identity the timeline shows for a user id that
    // is NOT the row's own sender (reactors, readers): member lookup, then
    // the LOCALPART. Mirrors senderDisplayName()'s fallback order — the
    // complete MXID is never the visible label, and a backend that answers
    // with the raw user id has told us "unresolved", not a display name.
    QString memberDisplayName(const QString &roomId,
                              const QString &userId) const;
    // The row's visible text. Redacted rows read as deleted, and a typed
    // profile-change row is PHRASED here rather than carrying a sentence in
    // `body` — the bridge leaves that field empty for those rows, so
    // anything still reading `body` directly would render nothing.
    QString visibleBodyFor(const TimelineEvent &e) const;
    // Answers DividerIntroducesVisibleContentRole for one divider row.
    // O(rows until the next divider), and it stops at the first drawn row.
    bool dividerIntroducesVisibleContent(int dividerRow) const;
    QVariantList pollAnswersVariant(const TimelineEvent &e) const;
    QVariantList readReceiptsVariant(const TimelineEvent &e) const;
    // Grouping is transparent through read markers and timeline-start, but a
    // DATE DIVIDER ends the run (one collapsed group must not span calendar
    // days under a single date separator). A visible message/media/call
    // event ends a group as always. See TimelineModel.cpp for the rationale.
    int stateGroupLeaderRow(int row) const;
    // First row of the run of redacted messages containing `row`, or -1 when
    // that row is not redacted. Mirrors stateGroupLeaderRow, including its
    // virtual-row rules (date dividers break, read markers do not).
    int deletedGroupLeaderRow(int row) const;
    int deletedGroupLengthFrom(int leaderRow) const;
    QVariantList stateGroupEntriesFrom(int leaderRow) const;
    // Re-read grouping roles only around a structural boundary. New rows
    // already query their correct roles on first bind; only their existing
    // neighbours (and a state-activity run crossing the boundary) can change.
    // A whole-model dataChanged here made every pagination page increasingly
    // expensive as loaded history grew and destabilized TableView geometry.
    void emitPresentationGroupingChanged(int first, int last);
    // Shared by the room-activity master switch and both of its halves, so a
    // sub-toggle can never refresh differently from the master.
    void refreshActivityPresentation();
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
    mutable QHash<QString, int> m_rowIndex;
    mutable bool m_rowIndexDirty = true;
    // Memoized MessageHtml::sanitize output per event id (FormattedBodyRole
    // is re-read for every row on member hydration; the sanitize walk is the
    // costly part). Invalidated per event on edit/replace/redact and
    // wholesale on member hydration, theme-color change, and reload.
    mutable QHash<QString, QString> m_sanitizedHtmlCache;
    // Memoized MessageSegmentsRole payload per event id. Derived from the
    // SAME inputs as m_sanitizedHtmlCache (formatted body, mention style,
    // member lookup), so the two are invalidated together through
    // forgetRenderedHtml()/clearRenderedHtml() rather than through a dozen
    // parallel remove() calls — this file already has eleven invalidation
    // sites, and a class that mutates state at N sites eventually misses
    // one (ReverseListProxyModel missed five of its own notify sites).
    // Only rows that really carry a code block get an entry: an ordinary
    // body is answered by a substring test with no sanitize walk at all.
    mutable QHash<QString, QVariantList> m_messageSegmentsCache;
    void forgetRenderedHtml(const QString &eventId);
    void clearRenderedHtml();

public:
    // v0.9 spoilers: flip one event's click-to-reveal state (delegate
    // routes the internal spoiler:toggle anchor here). Model-level so the
    // choice survives delegate churn; dies with the timeline.
    Q_INVOKABLE void toggleSpoilers(const QString &eventId);

private:
    QSet<QString> m_spoilersRevealed;
    QString m_selfUserId;
    QList<TimelineEvent> m_events;
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
    // Newest SDK-profile avatar this timeline has seen per sender. Receipt
    // chips resolve avatars through the member cache first; this catches
    // readers whose roster row is missing or unhydrated while their own
    // messages in the loaded timeline carry a profile avatar (live report
    // 2026-08-14: chips rendered letter fallbacks beside rows that showed
    // the same user's picture). Room-scoped: rebuilt on reload/room switch.
    QHash<QString, QString> m_senderAvatarIndex;
    void noteSenderAvatar(const TimelineEvent &event);
    void rebuildSenderAvatarIndex();
    QString m_typingText;

    // v0.6.1 loaded-timeline search (memory-only; never persisted).
    bool m_searchActive = false;
    QString m_searchQuery;
    QStringList m_searchResults;   // matching event ids, oldest → newest
    int m_searchIndex = -1;        // index into m_searchResults; -1 = none

    // Mention and link ink (validated OPAQUE #rrggbb strings; see
    // setMentionStyle). Empty until QML pushes the current theme.
    QString m_mentionAccentColor;
    QString m_mentionLinkColor;
    QString m_codeBackgroundColor;

    // Mirrors SettingsManager::showRoomActivity (default true, matching it).
    // Only DividerIntroducesVisibleContentRole reads it; nothing here
    // filters rows.
    bool m_showRoomActivity = true;
    // The two halves of "room activity", each defaulting TRUE so the split
    // is invisible to anyone who never opens it: master on + both halves on
    // is exactly the previous behaviour.
    bool m_showMembershipEvents = true;
    bool m_showProfileChangeEvents = true;
};
