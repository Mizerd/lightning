#pragma once

#include <algorithm>
#include <functional>

#include "matrix/TimelineEvent.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;
class UserProfileResolver;

class TimelineModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId NOTIFY roomIdChanged)
    Q_PROPERTY(int count READ eventCount NOTIFY countChanged)
    Q_PROPERTY(QString latestCallEventId READ latestCallEventId
               NOTIFY latestCallEventIdChanged)
    Q_PROPERTY(int realCount READ realEventCount NOTIFY countChanged)
    /// Source row of the SDK's read-marker ("New messages") virtual row, or -1
    /// when the loaded timeline does not carry one. See readMarkerRow().
    Q_PROPERTY(int firstUnreadRow READ readMarkerRow NOTIFY countChanged)
    Q_PROPERTY(QString typingText READ typingText NOTIFY typingTextChanged)
    Q_PROPERTY(bool canPaginate READ canPaginate NOTIFY paginationChanged)
    Q_PROPERTY(bool paginating READ paginating NOTIFY paginationChanged)
    // Last backward pagination failed; QML shows a Retry affordance.
    Q_PROPERTY(bool paginationFailed READ paginationFailed NOTIFY paginationChanged)
    // Find-in-loaded-messages: searches only events currently in this timeline
    // (main or thread), never server history or a persistent index.
    // Memory-only; cleared on endSearch() and on any room/thread switch.
    Q_PROPERTY(bool searchActive READ searchActive NOTIFY searchChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY searchChanged)
    Q_PROPERTY(int searchResultCount READ searchResultCount NOTIFY searchChanged)
    // 1-based position of the current match for display ("2 of 7"); 0 when
    // there are no matches.
    Q_PROPERTY(int searchCurrentPosition READ searchCurrentPosition
                   NOTIFY searchChanged)
    Q_PROPERTY(QString searchCurrentEventId READ searchCurrentEventId
                   NOTIFY searchChanged)
    // Whether routine room-activity rows render at all. Needed here because a
    // date divider's visibility depends on whether anything it introduces is
    // drawn (DividerIntroducesVisibleContentRole). Rows are never filtered
    // here; QML keeps the zero-height filter.
    Q_PROPERTY(bool showRoomActivity READ showRoomActivity
                   WRITE setShowRoomActivity NOTIFY showRoomActivityChanged)
    // The two halves of "room activity". Each defaults true and is subordinate
    // to the master switch.
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
        ThreadRootIdRole,        // Non-empty on thread replies.
        IsThreadRootRole,        // True if any event in this room has this as root.
        ThreadReplyCountRole,    // Number of visible replies for a thread root.
        // SDK thread-summary presentation roles (thread roots only).
        ThreadLatestPreviewRole,   // Sanitized preview of the latest reply.
        ThreadLatestKindRole,      // Semantic kind for a safe label (image/gif/…).
        ThreadLatestSenderRole,    // MXID of the latest reply's sender.
        ThreadLatestSenderDisplayNameRole, // Friendly name (falls back to MXID).
        ThreadLatestSenderAvatarMxcRole,   // mxc:// via the safe avatar path.
        ThreadLatestTimestampRole, // QDateTime of the latest reply.
        ThreadUnreadRole,          // Conservative receipt-based unread hint.
        // m.mentions metadata (SDK-parsed).
        MentionsMeRole,
        MentionsRoomRole,
        // Encryption metadata, so MessageDelegate can style undecryptable rows
        // without matching body strings.
        IsEncryptedRole,
        IsDecryptedRole,
        UndecryptableRole,
        ErrorKindRole,
        // Live SDK timeline roles.
        ItemIdRole,          // Stable SDK item id (survives in-place updates).
        IsLocalEchoRole,     // True until the remote echo reconciles.
        SendErrorRole,       // Coarse category when status == Failed.
        // Upload progress while Sending: 0.0-1.0 once the SDK reports a total,
        // else -1 ("extent unknown"), which draws an indeterminate bar.
        UploadProgressRole,
        IsVirtualRole,       // Date divider / read marker / timeline start.
        // Media bridge and identity presentation.
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
        // Consecutive redacted rows collapse like a state run: the first row is
        // the leader and carries the run's length; the rest render nothing.
        DeletedGroupLeaderRole,
        DeletedGroupCountRole,
        SenderAvatarMxcRole,
        SenderInitialsRole,
        BeginsSenderGroupRole,
        ContinuesSenderGroupRole,
        EndsSenderGroupRole,
        ShowSenderIdentityRole,
        StableEventIdRole,
        // Typed media presentation: video/audio/voice/sticker rows reserve
        // correct geometry before any bytes arrive.
        IsVideoRole,
        IsAudioRole,
        IsStickerRole,
        MediaDurationMsRole,
        MediaIsVoiceRole,
        MediaWaveformRole,   // real MSC3245 envelope (0..100); may be empty
        // MSC3381 polls. Aggregation is SDK-owned; counts are 0 while an
        // undisclosed poll runs.
        IsPollRole,
        PollQuestionRole,
        PollKindRole,           // "disclosed" | "undisclosed"
        PollMaxSelectionsRole,
        PollAnswersRole,        // list of {id, text, count, byMe}
        PollTotalVotersRole,
        PollEndedRole,
        CanEndPollRole,         // own poll, not ended (conservative rule)
        // Shared location. `locationHasPoint` gates the others: an unparsed geo
        // URI leaves coordinates absent rather than at 0,0.
        IsLocationRole,
        LocationHasPointRole,
        LocationLatRole,
        LocationLonRole,
        LocationUncertaintyRole,
        LocationDescriptionRole,
        LocationAssetRole,
        LocationLiveRole,
        LocationLiveActiveRole,
        // Read-receipt chips: other users whose receipt points here, newest
        // first, as {userId, displayName, avatarMxc, tsMs}. Only the local user
        // is excluded (Element convention); a user's receipt shows even on
        // their own message, which is how a DM says "read up to here". Names
        // and avatars resolve through the member lookup. Thread timelines
        // always answer empty: their SDK receipt tracking is disabled because
        // it is not thread-aware.
        ReadReceiptsRole,
        // Total other readers for the "+N" chip (uncapped count minus self), >=
        // the list size. The FFI window is capped at 16, so a hidden
        // self-receipt can overcount by one in larger rooms, never undercount.
        ReadReceiptsTotalRole,
        // Fenced code blocks: ordered {kind, text, language} maps (kind 0 rich
        // text, 1 code block) for bodies that contain one; empty otherwise, so
        // ordinary rows keep the single-TextEdit path. See
        // MessageHtml::segments().
        MessageSegmentsRole,
        // On DateDivider rows: true when at least one row before the next
        // divider is drawn; an orphan date label must take no space. Always
        // true on other rows so QML can read it unconditionally.
        DividerIntroducesVisibleContentRole,
        // ── Typed call rows ─────────────────────────────────────────────── A
        // call is room history and draws its own row (CallEventDelegate.qml)
        // instead of joining the collapsed "N room updates" group. True for the
        // Rust `call` row and for the legacy StateChange with stateKind
        // "m.call" / "m.call.video".
        IsCallEventRole,
        // The finished sentence, translated and using the actor's resolved
        // name. The bridge sends an empty body so the sentence can be
        // localized.
        CallEventTextRole,
        // The caller's stated video intent. False means "not known to be
        // video", never "audio only".
        CallIsVideoRole,
        // How many people declined. A count only; decliner ids never cross the
        // FFI.
        CallDeclinedCountRole,
        // ── MSC4274 galleries and the reply target's kind ──── Every
        // attachment of a gallery row (two or more in one event) as {mediaKey,
        // kind, filename, mimetype, size, width, height, durationMs,
        // thumbAvailable} in the sender's order; empty for other rows. The
        // row's own media roles describe its primary item.
        GalleryItemsRole,
        // The replied-to event's kind ("image", "file", …; empty when not
        // loaded) and, for a gallery, its attachment count, so a quote without
        // words can say "Image" / "2 images".
        ReplyToKindRole,
        ReplyToCountRole,
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    // Global profiles for mention targets the member snapshot cannot name.
    // Optional; without it an unknown target shows its localpart.
    void setProfileResolver(UserProfileResolver *resolver);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    // Row count as the view sees it. Also PaginationController's progress
    // measure (batchRowGrowth()): every delivered row is exposed immediately.
    int eventCount() const { return static_cast<int>(m_events.size()); }

    /// Event id of the newest loaded call row, or empty. MatrixRTC has one
    /// session per room, so only the newest call row offers Join; older rows
    /// read as history.
    QString latestCallEventId() const { return m_latestCallEventId; }
    /// Loaded rows that are real events, excluding date dividers, the read
    /// marker and the timeline-start row. `count` counts rows and must not be
    /// used for "how many messages" labels. O(rows) and deliberately uncached:
    /// it is read by one label on a small thread timeline.
    int realEventCount() const
    {
        return static_cast<int>(std::count_if(
            m_events.begin(), m_events.end(),
            [](const TimelineEvent &e) { return !e.isVirtual(); }));
    }
    /// The loaded rows, virtual ones included, by reference (the export
    /// renderer walks them). Do not hold across a model change.
    const QList<TimelineEvent> &events() const { return m_events; }

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

    // Whether a routine state row of this kind is drawn. Public so the filter
    // matrix is testable without a backend. The QML row filter
    // (qml/MessageDelegate.qml, roomActivityVisible) applies the same matrix;
    // keep them in step.
    bool activityKindVisible(const QString &stateKind) const;

    // Sentence for a typed m.room.member profile change (stateKind
    // "member_profile"), which the bridge leaves unphrased so it can be
    // translated and use the actor's resolved name. Old/new names are untrusted
    // plain text, rendered as PlainText. Static so it is testable without a
    // model.
    static QString profileChangeDescription(const TimelineEvent &e,
                                            const QString &actorDisplayName);

    // Whether a row is a call somebody started, in either shape: the Rust
    // `call` row or the legacy "state event with kind m.call" (older bridges,
    // mock/HTTP). Static and stateless so every reader asks one question.
    static bool isCallEventRow(const TimelineEvent &e);
    /// Recomputes latestCallEventId() and emits when it moves. Cheap: scans
    /// from the newest end and stops at the first call row.
    void refreshLatestCallEvent();
    // Sentence for a call row, built here with the actor's resolved name (the
    // bridge sends none); same contract as profileChangeDescription.
    static QString callEventDescription(const TimelineEvent &e,
                                        const QString &actorDisplayName);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void requestOlder();
    Q_INVOKABLE void markVisibleAsRead(int firstVisibleRow, int lastVisibleRow);
    Q_INVOKABLE QString ownUserId() const { return m_selfUserId; }

    // Newest event that may receive a read receipt: skips virtual rows, local
    // echoes without a remote id and failed sends. Optionally reports its
    // timestamp so callers never regress to an older receipt.
    QString latestReadableEventId(qint64 *timestampMs = nullptr) const;

    // Stable anchors for backward-pagination scroll preservation. stableIdAt
    // prefers the SDK item id (survives in-place updates); rowForStableId
    // matches either.
    Q_INVOKABLE QString stableIdAt(int row) const;
    // Matrix event id only; unlike the SDK item id it survives a rebuilt live
    // timeline.
    Q_INVOKABLE QString eventIdAt(int row) const;
    Q_INVOKABLE int rowForStableId(const QString &stableId) const;
    // Presentation-safe subset for rows not yet loaded by the view, so media
    // geometry can be reserved from Matrix `info` metadata. QML still owns all
    // layout arithmetic.
    Q_INVOKABLE QVariantMap layoutMetadataAt(int row) const;

    // Stable-id action helpers; each re-resolves the event so a recycled
    // delegate cannot act on another row.
    Q_INVOKABLE QString visibleTextForEvent(const QString &eventId) const;
    // Media-bridge key for replying to an image event (empty otherwise), so the
    // composer banner shows the same thumbnail as the quote.
    Q_INVOKABLE QString mediaKeyForEvent(const QString &eventId) const;
    /// How an inline custom emoji's `mxc:` source becomes renderable. Never
    /// applied to the memoized sanitizer output; see
    /// MessageHtml::resolveInlineImages.
    void setInlineImageResolver(
        std::function<QString(const QString &mxcUri)> resolve);
    /// Media arrived: re-read formatted bodies carrying emoji. A no-op when
    /// there are none.
    void notifyInlineImagesChanged();

    // Sanitized formatted body (as FormattedBodyRole) for the edit flow:
    // plain display text carries no mention markdown, so the composer
    // recovers mention refs from the sanitized mention: anchors.
    Q_INVOKABLE QString sanitizedHtmlForEvent(const QString &eventId) const;

    // Loaded-timeline search. beginSearch/updateSearch recompute matches over
    // the loaded visible text; next/prev walk them (wrapping); endSearch clears
    // all state. Case-insensitive.
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
    // The real Matrix room id for an event; thread timelines carry the
    // composite id, which must never reach a protocol call.
    Q_INVOKABLE QString realRoomIdForEvent(const QString &eventId) const;
    /// Source row of the SDK's read-marker ("New messages") virtual row, or -1
    /// when the loaded timeline does not carry one.
    ///
    /// NOTIFY countChanged is exact: a virtual row only moves by insertion or
    /// removal, and every mutation ends in countChanged().
    ///
    /// The row is the marker; there is no event id to jump to. The SDK places
    /// it from `m.fully_read`, so its absence means either nothing unread or a
    /// marker beyond the loaded window, which the caller resolves by
    /// paginating.
    Q_INVOKABLE int readMarkerRow() const;
    // Loaded-timeline composition for the opt-in scroll trace
    // (LIGHTNING_SCROLL_TRACE). Counts only; O(rows).
    Q_INVOKABLE int stateActivityRowCount() const;
    Q_INVOKABLE int stateGroupCount() const;
    Q_INVOKABLE QVariantMap messageDetails(const QString &eventId) const;
    // Edit history and event source for this timeline's room, answered on the
    // signals below. Display data held by the open dialog; never cached.
    Q_INVOKABLE void requestEditHistory(const QString &eventId);
    Q_INVOKABLE void requestEventSource(const QString &eventId);
    // Sanitize an arbitrary formatted body exactly as rows are (mention style
    // and name resolver), for the edit-history dialog's untrusted revisions.
    Q_INVOKABLE QString sanitizeHtml(const QString &html) const;
    Q_INVOKABLE bool canEditEvent(const QString &eventId) const;
    Q_INVOKABLE bool canRedactEvent(const QString &eventId) const;
    /// Delete and react through the model showing the event, since only it
    /// knows which timeline that is: in a thread this model's id is the
    /// composite, and the live room timeline (hide_threaded_events) cannot find
    /// thread replies.
    Q_INVOKABLE void redactEvent(const QString &eventId,
                                 const QString &reason = QString());
    Q_INVOKABLE void toggleReaction(const QString &eventId, const QString &key);

    // Retry a failed local echo with a transaction id through the backend send
    // queue; the SDK re-attempts the same item, so no duplicate appears.
    Q_INVOKABLE void retrySend(int row);
    // Discard a local echo that has not reached the server, while Sending or
    // Failed. The row disappears only when the backend confirms the abort.
    Q_INVOKABLE bool canCancelSend(int row) const;
    Q_INVOKABLE void cancelSend(int row);

    // Manual decryption retry for this timeline's unable-to-decrypt events
    // (no-op without a crypto machine).
    Q_INVOKABLE void retryDecryption();

    // Loaded image events, oldest first, for the image viewer's navigation.
    // Each entry: {row, mediaKey, filename, sender, timestamp, mime, httpUrl}.
    // No history is fetched.
    Q_INVOKABLE QVariantList imageEntries() const;
    // All loaded media events (images and files), oldest first, for Room
    // Information's "Media & Files". Adds isImage and size to imageEntries().
    Q_INVOKABLE QVariantList mediaEntries() const;

    // Timeline mention and link ink, pushed from QML (AppTheme is the sole
    // token source). accentColor marks mentions of the local user; linkColor
    // covers other mentions and URLs (empty falls back to accentColor). Values
    // must be opaque #rrggbb; anything else degrades to Qt's default link look.
    // Live theme switches re-announce FormattedBodyRole. softColor is unused
    // and kept for the QML call's arity.
    Q_INVOKABLE void setMentionStyle(const QString &accentColor,
                                     const QString &softColor,
                                     const QString &codeBackground = QString(),
                                     const QString &linkColor = QString());

    // Styles the literal "@room" in a body that carries m.mentions.room, for
    // both formatted and linkified plain bodies. The caller owns the
    // m.mentions.room test; never apply this to a body that merely contains the
    // words.
    Q_INVOKABLE QString markRoomMention(const QString &safeHtml) const;

Q_SIGNALS:
    // SENSITIVE in encrypted rooms (plaintext bodies, decrypted JSON): display
    // and drop, never log or store. `partial` means not the whole history; see
    // MatrixClient::editHistoryReceived.
    void editHistoryReceived(const QString &eventId, bool ok, bool partial,
                             const QVariantList &revisions);
    void eventSourceReceived(const QString &eventId, bool ok,
                             const QString &json, const QVariantMap &encryption);
    void roomIdChanged();
    void countChanged();
    void latestCallEventIdChanged();
    void typingTextChanged();
    void paginationChanged();
    // A backward-pagination batch prepended `count` rows, shifting existing
    // rows by that amount. Fired once per batch.
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
    // Index-based diff application. Every index is validated; on mismatch the
    // model reloads the backend's timeline rather than corrupting state.
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
    // Re-render only rows whose mention pills would now read differently and
    // announce them. Shared by member hydration and the profile resolver.
    int refreshStaleMentionRows();
    void onPaginationStateChanged(const QString &roomId);

private:
    const TimelineEvent *eventForId(const QString &eventId) const;
    // Recompute search matches over the loaded timeline, preserving the
    // currently selected match's event id when it still matches.
    void recomputeSearch();
    void reload();
    int rowForEventId(const QString &eventId) const;
    // Lazily rebuilt eventId -> row map behind rowForEventId/eventForId. Every
    // structural mutation (and whole-event replacement, which can rename local:
    // -> remote) calls invalidateRowIndex(); the next lookup rebuilds in O(n).
    void invalidateRowIndex() const { m_rowIndexDirty = true; }
    // Newest row carrying each user's read receipt. A user has read up to one
    // position, so a host shows a reader only when no newer row carries them.
    const QHash<QString, int> &latestReceiptRows() const;
    mutable QHash<QString, int> m_latestReceiptRow;
    mutable bool m_latestReceiptRowDirty = true;
    void invalidateReceiptIndex() const { m_latestReceiptRowDirty = true; }
    /// Each reader's current position as an event id, for announceReceiptHost.
    QHash<QString, QString> receiptPositionsByEvent() const;
    const QHash<QString, int> &rowIndex() const;
    void refreshTypingText();
    QVariantList reactionsVariant(const TimelineEvent &e) const;
    // Resolves identities other than the row's own sender (reactors, readers):
    // member lookup, then the localpart. A raw user id from the backend means
    // unresolved, never a display name.
    QString memberDisplayName(const QString &roomId,
                              const QString &userId) const;
    // The row's visible text. Redacted rows read as deleted, and typed
    // profile-change rows are phrased here because the bridge leaves `body`
    // empty.
    QString visibleBodyFor(const TimelineEvent &e) const;
    // Answers DividerIntroducesVisibleContentRole. O(rows to the next divider),
    // stopping at the first drawn row.
    bool dividerIntroducesVisibleContent(int dividerRow) const;
    QVariantList pollAnswersVariant(const TimelineEvent &e) const;
    QVariantList readReceiptsVariant(const TimelineEvent &e) const;
    // The merged view for a hosting row: its own receipts plus those of
    // every following row it hosts, one entry per reader, newest first.
    QList<ReadReceipt> hostedReceipts(int row, int *reportedTotal) const;
    /// Announce every host whose receipts the mutation at `row` may have
    /// changed: the row's own host, its neighbour above, and the hosts each
    /// reader left and reached, found by comparing `before` (captured by event
    /// id ahead of the mutation) with the rebuilt index. `hostingChanged`: a
    /// self-hosting row appeared, vanished or flipped, so the neighbour above
    /// is announced too. `announceRow`: the caller does not emit for `row` (a
    /// removal). Otherwise a row is announced only when a reader's position
    /// moved.
    void announceReceiptHost(int row, const QHash<QString, QString> &before,
                             bool hostingChanged, bool announceRow);
    // Grouping is transparent through read markers and timeline start, but a
    // date divider ends the run; a visible message/media/call event always
    // does.
    int stateGroupLeaderRow(int row) const;
    // First row of the run of redacted messages containing `row`, or -1 when
    // that row is not redacted. Mirrors stateGroupLeaderRow, including its
    // virtual-row rules (date dividers break, read markers do not).
    int deletedGroupLeaderRow(int row) const;
    int deletedGroupLengthFrom(int leaderRow) const;
    QVariantList stateGroupEntriesFrom(int leaderRow) const;

    // ── Read-receipt hosting ─────────────────────────────────────────────
    //
    // The SDK attaches a receipt to the newest event read, often a row that
    // draws no body (call-membership updates, folded state rows, hidden
    // activity). Such rows host nothing; their receipts show on the nearest
    // drawn row above, and a hosting row's ReadReceipts roles merge every row
    // it hosts.
    bool rowHostsReceipts(int row) const;
    int receiptHostRow(int row) const;
    // Re-read grouping roles only around a structural boundary: new rows query
    // their roles on first bind, so only existing neighbours (and a state run
    // crossing the boundary) can change. A whole-model dataChanged grows with
    // history and destabilizes view geometry.
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
    // True when an in-place update changed a field grouping reads, so a Set
    // refreshes neighbours only when it must.
    bool groupingInputsDiffer(const TimelineEvent &before,
                              const TimelineEvent &after) const;
    QUrl mediaHttp(const QString &mxc) const;
    QUrl mediaThumbHttp(const QString &mxc, int w, int h) const;

    MatrixClient *m_client = nullptr;
    QString m_roomId;
    // The room id every room-keyed lookup must use. A thread model's m_roomId
    // is the composite `room ␟ thread ␟ root` (what addresses its diff stream),
    // and nothing room-keyed (member cache, membersChanged) matches it. Equal
    // to m_roomId for an ordinary room. Written only beside m_roomId.
    QString m_realRoomId;
    mutable QHash<QString, int> m_rowIndex;
    mutable bool m_rowIndexDirty = true;
    // Memoized MessageHtml::sanitize output per event id. Invalidated per event
    // on edit/replace/redact and wholesale on hydration, theme-colour change
    // and reload.
    mutable QHash<QString, QString> m_sanitizedHtmlCache;
    // Every (user id -> name used) pair a cached sanitize resolved, per event,
    // so member hydration re-checks those pairs and forgets only rows whose
    // answer changed instead of rebuilding every row.
    mutable QHash<QString, QList<QPair<QString, QString>>> m_htmlMemberDeps;
    UserProfileResolver *m_profiles = nullptr;
    // The name a mention pill shows for `userId`: the room member name, else
    // the resolver's global profile, else empty (the sanitizer prints the
    // localpart). `ask` lets a render request an unknown profile; comparisons
    // must not.
    QString mentionNameFor(const QString &userId, bool ask) const;
    std::function<QString(const QString &)> m_inlineImageResolver;
    /// True once any loaded formatted body carries an emoticon, so media
    /// arrival costs nothing otherwise.
    mutable bool m_hasInlineEmoji = false;
    // Memoized MessageSegmentsRole payload per event id. Same inputs as
    // m_sanitizedHtmlCache, so both are invalidated together through
    // forgetRenderedHtml()/clearRenderedHtml(). Only rows with a code block get
    // an entry.
    mutable QHash<QString, QVariantList> m_messageSegmentsCache;
    void forgetRenderedHtml(const QString &eventId);
    void clearRenderedHtml();

public:
    // Flip one event's spoiler reveal (the delegate routes the internal
    // spoiler:toggle anchor here). Model-level so it survives delegate churn;
    // dies with the timeline.
    Q_INVOKABLE void toggleSpoilers(const QString &eventId);

private:
    QSet<QString> m_spoilersRevealed;
    QString m_selfUserId;
    QList<TimelineEvent> m_events;
    /// Cache for latestCallEventId(); see refreshLatestCallEvent().
    QString m_latestCallEventId;
    // Loaded thread replies per root event id, so IsThreadRootRole and
    // ThreadReplyCountRole answer in O(1) instead of scanning the event list
    // per row. Rebuilt once per structural mutation.
    QHash<QString, int> m_threadReplyCounts;
    void rebuildThreadReplyIndex();
    // Newest SDK-profile avatar seen per sender, a fallback for receipt chips
    // whose reader is missing from the member cache. Rebuilt on reload/room
    // switch.
    QHash<QString, QString> m_senderAvatarIndex;
    void noteSenderAvatar(const TimelineEvent &event);
    void rebuildSenderAvatarIndex();
    QString m_typingText;

    // Loaded-timeline search (memory-only; never persisted).
    bool m_searchActive = false;
    QString m_searchQuery;
    QStringList m_searchResults;   // matching event ids, oldest → newest
    int m_searchIndex = -1;        // index into m_searchResults; -1 = none

    // Mention and link ink (validated opaque #rrggbb; see setMentionStyle).
    // Empty until QML pushes the theme.
    QString m_mentionAccentColor;
    QString m_mentionLinkColor;
    QString m_codeBackgroundColor;

    // Mirrors SettingsManager::showRoomActivity. Only
    // DividerIntroducesVisibleContentRole reads it; no rows are filtered here.
    bool m_showRoomActivity = true;
    // The two halves of "room activity", each defaulting true.
    bool m_showMembershipEvents = true;
    bool m_showProfileChangeEvents = true;
};
