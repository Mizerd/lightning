#pragma once

#include "matrix/TimelineEvent.h"

#include <QAbstractListModel>
#include <QList>
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

public:
    enum Roles {
        EventIdRole = Qt::UserRole + 1,
        SenderRole,
        SenderDisplayNameRole,
        BodyRole,
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
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    int eventCount() const { return static_cast<int>(m_events.size()); }

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

Q_SIGNALS:
    void roomIdChanged();
    void countChanged();
    void typingTextChanged();
    void paginationChanged();
    // v0.5.11: a backward-pagination batch prepended `count` rows; existing
    // rows shifted down by exactly that amount.
    void olderPrepended(int count);
    void searchChanged();

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
    const TimelineEvent *eventForId(const QString &eventId) const;
    // Recompute search matches over the loaded timeline, preserving the
    // currently selected match's event id when it still matches.
    void recomputeSearch();
    void reload();
    int rowForEventId(const QString &eventId) const;
    void refreshTypingText();
    QVariantList reactionsVariant(const TimelineEvent &e) const;
    // Grouping is transparent through virtual rows (date dividers, read
    // markers, timeline-start) — only a visible message/media event ends a
    // group. See TimelineModel.cpp for the rationale.
    int stateGroupLeaderRow(int row) const;
    QVariantList stateGroupEntriesFrom(int leaderRow) const;
    void emitPresentationGroupingChanged();
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
    QString m_typingText;

    // v0.6.1 loaded-timeline search (memory-only; never persisted).
    bool m_searchActive = false;
    QString m_searchQuery;
    QStringList m_searchResults;   // matching event ids, oldest → newest
    int m_searchIndex = -1;        // index into m_searchResults; -1 = none
};
