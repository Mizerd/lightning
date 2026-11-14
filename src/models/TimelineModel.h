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
    Q_PROPERTY(int firstUnreadRow READ readMarkerRow NOTIFY countChanged)
    Q_PROPERTY(bool firstUnreadShown READ firstUnreadShown
               NOTIFY firstUnreadShownChanged)
    Q_PROPERTY(QString typingText READ typingText NOTIFY typingTextChanged)
    Q_PROPERTY(bool canPaginate READ canPaginate NOTIFY paginationChanged)
    Q_PROPERTY(bool paginating READ paginating NOTIFY paginationChanged)
    Q_PROPERTY(bool paginationFailed READ paginationFailed NOTIFY paginationChanged)
    Q_PROPERTY(bool searchActive READ searchActive NOTIFY searchChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery NOTIFY searchChanged)
    Q_PROPERTY(int searchResultCount READ searchResultCount NOTIFY searchChanged)
    Q_PROPERTY(int searchCurrentPosition READ searchCurrentPosition
                   NOTIFY searchChanged)
    Q_PROPERTY(QString searchCurrentEventId READ searchCurrentEventId
                   NOTIFY searchChanged)
    Q_PROPERTY(bool showRoomActivity READ showRoomActivity
                   WRITE setShowRoomActivity NOTIFY showRoomActivityChanged)
    Q_PROPERTY(bool showMembershipEvents READ showMembershipEvents
                   WRITE setShowMembershipEvents
                   NOTIFY showMembershipEventsChanged)
    Q_PROPERTY(bool showProfileChangeEvents READ showProfileChangeEvents
                   WRITE setShowProfileChangeEvents
                   NOTIFY showProfileChangeEventsChanged)
    Q_PROPERTY(bool threadRepliesInline READ threadRepliesInline
                   WRITE setThreadRepliesInline
                   NOTIFY threadPresentationChanged)
    Q_PROPERTY(QString threadFilterRootId READ threadFilterRootId
                   WRITE setThreadFilterRootId
                   NOTIFY threadPresentationChanged)
    Q_PROPERTY(bool hideThreadReplies READ hideThreadReplies
                   WRITE setHideThreadReplies
                   NOTIFY threadPresentationChanged)

public:
    enum Roles {
        EventIdRole = Qt::UserRole + 1,
        SenderRole,
        SenderDisplayNameRole,
        BodyRole,
        FormattedBodyRole,
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
        ReplyToRedactedRole,
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
        ThreadRootIdRole,
        IsThreadRootRole,
        ThreadReplyCountRole,
        ThreadReplyAwaitingKeysRole,
        ThreadLatestPreviewRole,
        ThreadLatestKindRole,
        ThreadLatestSenderRole,
        ThreadLatestSenderDisplayNameRole,
        ThreadLatestSenderAvatarMxcRole,
        ThreadLatestTimestampRole,
        ThreadUnreadRole,
        MentionsMeRole,
        MentionsRoomRole,
        RoomMentionHighlightedRole,
        PushHighlightedRole,
        PushVerdictKnownRole,
        IsEncryptedRole,
        IsDecryptedRole,
        UndecryptableRole,
        ErrorKindRole,
        ItemIdRole,
        IsLocalEchoRole,
        SendErrorRole,
        UploadProgressRole,
        IsVirtualRole,
        MediaKeyRole,
        MediaSourceAvailableRole,
        MediaThumbAvailableRole,
        MediaSpoilerRole,
        MediaSpoilerReasonRole,
        SenderNameAmbiguousRole,
        SameSenderAsPreviousRole,
        IsStateActivityRole,
        IsRoutineActivityRole,
        StateKindRole,
        StateGroupIdRole,
        StateGroupLeaderRole,
        StateGroupEntriesRole,
        DeletedGroupLeaderRole,
        DeletedGroupCountRole,
        SenderAvatarMxcRole,
        SenderInitialsRole,
        BeginsSenderGroupRole,
        ContinuesSenderGroupRole,
        EndsSenderGroupRole,
        ShowSenderIdentityRole,
        StableEventIdRole,
        IsVideoRole,
        IsAudioRole,
        IsStickerRole,
        MediaDurationMsRole,
        MediaIsVoiceRole,
        MediaWaveformRole,
        IsPollRole,
        PollQuestionRole,
        PollKindRole,
        PollMaxSelectionsRole,
        PollAnswersRole,
        PollTotalVotersRole,
        PollEndedRole,
        CanEndPollRole,
        IsLocationRole,
        LocationHasPointRole,
        LocationLatRole,
        LocationLonRole,
        LocationUncertaintyRole,
        LocationDescriptionRole,
        LocationAssetRole,
        LocationLiveRole,
        LocationLiveActiveRole,
        ReadReceiptsRole,
        ReadReceiptsTotalRole,
        MessageSegmentsRole,
        DividerIntroducesVisibleContentRole,
        IsCallEventRole,
        CallEventTextRole,
        CallIsVideoRole,
        CallDeclinedCountRole,
        GalleryItemsRole,
        ReplyToKindRole,
        ReplyToCountRole,
        SentReceiptRole,
    };

    explicit TimelineModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setProfileResolver(UserProfileResolver *resolver);

    QString roomId() const { return m_roomId; }
    void setRoomId(const QString &roomId);
    int eventCount() const { return static_cast<int>(m_events.size()); }

    QString latestCallEventId() const { return m_latestCallEventId; }
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
    bool threadRepliesInline() const { return m_threadRepliesInline; }
    void setThreadRepliesInline(bool on);
    QString threadFilterRootId() const { return m_threadFilterRootId; }
    void setThreadFilterRootId(const QString &rootEventId);
    bool hideThreadReplies() const { return m_hideThreadReplies; }
    static constexpr int kDefaultSenderGroupWindowSeconds = 5 * 60;
    int senderGroupWindowSeconds() const { return m_senderGroupWindowSeconds; }
    void setSenderGroupWindowSeconds(int seconds);
    void setHideThreadReplies(bool hide);
    Q_INVOKABLE void clearThreadFilters();
    // Re-read the room from a backend that answers timeline() anew (the mock
    // and HTTP backends) after it changed what that answer contains.
    void reloadFromBackend() { reload(); }

    bool activityKindVisible(const QString &stateKind) const;

    static QString profileChangeDescription(const TimelineEvent &e,
                                            const QString &actorDisplayName);

    // Whether a row is a call somebody started, in either shape: the Rust
    // `call` row or the legacy "state event with kind m.call" (older bridges,
    // mock/HTTP). Static and stateless so every reader asks one question.
    static bool isCallEventRow(const TimelineEvent &e);
    bool isThreadReplyAwaitingKeys(const TimelineEvent &e) const;
    void refreshLatestCallEvent();
    static QString callEventDescription(const TimelineEvent &e,
                                        const QString &actorDisplayName);

    void retranslate();

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void requestOlder();
    Q_INVOKABLE void markVisibleAsRead(int firstVisibleRow, int lastVisibleRow);
    Q_INVOKABLE QString ownUserId() const { return m_selfUserId; }

    QString latestReadableEventId(qint64 *timestampMs = nullptr) const;

    Q_INVOKABLE QString stableIdAt(int row) const;
    Q_INVOKABLE QString eventIdAt(int row) const;
    Q_INVOKABLE int rowForStableId(const QString &stableId) const;
    Q_INVOKABLE QVariantMap layoutMetadataAt(int row) const;

    Q_INVOKABLE QString visibleTextForEvent(const QString &eventId) const;
    Q_INVOKABLE QStringList messageIdsBetween(const QString &firstId,
                                              const QString &secondId) const;
    Q_INVOKABLE QString plainTextForMessages(const QStringList &eventIds,
                                             const QString &timeFormat) const;
    Q_INVOKABLE QString replyPreviewForEvent(const QString &eventId) const;
    Q_INVOKABLE QString mediaKeyForEvent(const QString &eventId) const;
    void setInlineImageAspect(std::function<double(const QString &source)> aspect);
    void setInlineImageResolver(
        std::function<QString(const QString &mxcUri)> resolve);
    void notifyInlineImagesChanged();

    Q_INVOKABLE QString sanitizedHtmlForEvent(const QString &eventId) const;

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
    Q_INVOKABLE QString realRoomIdForEvent(const QString &eventId) const;
    bool isEventRedacted(const QString &roomId, const QString &eventId) const;
    QVariantMap nextVoiceMessage(const QString &roomId,
                                 const QString &afterEventId) const;
    QVariantList voiceRunAfter(const QString &roomId,
                               const QString &afterEventId,
                               const QString &threadRootId = {},
                               int max = 50) const;
    Q_INVOKABLE int readMarkerRow() const;
    bool firstUnreadShown() const;
    Q_INVOKABLE int stateActivityRowCount() const;
    Q_INVOKABLE int stateGroupCount() const;
    Q_INVOKABLE QVariantMap messageDetails(const QString &eventId) const;
    Q_INVOKABLE QVariantMap cumulativeReadersForEvent(const QString &eventId) const;
    Q_INVOKABLE void requestEditHistory(const QString &eventId);
    Q_INVOKABLE void requestEventSource(const QString &eventId);
    Q_INVOKABLE QString sanitizeHtml(const QString &html) const;
    Q_INVOKABLE bool canEditEvent(const QString &eventId) const;
    Q_INVOKABLE bool canRedactEvent(const QString &eventId,
                                    bool canRedactOthers = false) const;
    Q_INVOKABLE void redactEvent(const QString &eventId,
                                 const QString &reason = QString());
    Q_INVOKABLE void toggleReaction(const QString &eventId, const QString &key);

    Q_INVOKABLE void retrySend(int row);
    Q_INVOKABLE bool canCancelSend(int row) const;
    Q_INVOKABLE void cancelSend(int row);

    Q_INVOKABLE void retryDecryption();

    Q_INVOKABLE QVariantList imageEntries() const;
    Q_INVOKABLE QVariantList mediaEntries() const;

    Q_INVOKABLE void setMentionStyle(const QString &accentColor,
                                     const QString &softColor,
                                     const QString &codeBackground = QString(),
                                     const QString &linkColor = QString(),
                                     bool underlineLinks = false);

    Q_INVOKABLE QString markRoomMention(const QString &safeHtml,
                                        const QString &fill,
                                        const QString &ink) const;

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
    void firstUnreadShownChanged();
    void latestCallEventIdChanged();
    void typingTextChanged();
    void paginationChanged();
    void olderPrepended(int count);
    void searchChanged();
    void showRoomActivityChanged();
    void showMembershipEventsChanged();
    void showProfileChangeEventsChanged();
    void threadPresentationChanged();

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
    int refreshStaleMentionRows();
    void onPaginationStateChanged(const QString &roomId);

private:
    const TimelineEvent *eventForId(const QString &eventId) const;
    const TimelineEvent *loadedReplyTarget(const TimelineEvent &reply) const;
    void refreshQuotesOfRepliesTo(const QSet<QString> &inserted);
    void recomputeSearch();
    void reload();
    int rowForEventId(const QString &eventId) const;
    // Lazily rebuilt eventId -> row map behind rowForEventId/eventForId. Every
    // structural mutation (and whole-event replacement, which can rename local:
    // -> remote) calls invalidateRowIndex(); the next lookup rebuilds in O(n).
    void invalidateRowIndex() const { m_rowIndexDirty = true; }
    const QHash<QString, int> &latestReceiptRows() const;
    mutable QHash<QString, int> m_latestReceiptRow;
    mutable bool m_latestReceiptRowDirty = true;
    void invalidateReceiptIndex() const { m_latestReceiptRowDirty = true; }
    QHash<QString, QString> receiptPositionsByEvent() const;
    const QHash<QString, int> &rowIndex() const;
    void refreshTypingText();
    QVariantList reactionsVariant(const TimelineEvent &e) const;
    QString memberDisplayName(const QString &roomId,
                              const QString &userId) const;
    QString readerDisplayName(const QString &roomId,
                              const QString &userId) const;
    QString readerAvatarMxc(const QString &roomId,
                            const QString &userId) const;
    void refreshReceiptReader(const QString &userId);
    QString visibleBodyFor(const TimelineEvent &e) const;
    bool dividerIntroducesVisibleContent(int dividerRow) const;
    QVariantList pollAnswersVariant(const TimelineEvent &e) const;
    QVariantList readReceiptsVariant(const TimelineEvent &e) const;
    QList<ReadReceipt> hostedReceipts(int row, int *reportedTotal) const;
    void announceReceiptHost(int row, const QHash<QString, QString> &before,
                             bool hostingChanged, bool announceRow);
    int stateGroupLeaderRow(int row) const;
    int deletedGroupLeaderRow(int row) const;
    int deletedGroupLengthFrom(int leaderRow) const;
    QVariantList stateGroupEntriesFrom(int leaderRow) const;

    bool rowHostsReceipts(int row) const;
    int receiptHostRow(int row) const;
    int sentReceiptRow() const;
    void refreshSentReceipt();
    QString m_sentReceiptEventId;
    bool m_refreshingSentReceipt = false;
    void emitPresentationGroupingChanged(int first, int last);
    void refreshActivityPresentation();
    QString senderDisplayName(const TimelineEvent &event) const;
    QString senderInitials(const TimelineEvent &event) const;
    bool isVisualMessage(const TimelineEvent &event) const;
    int previousMessageRowForGrouping(int row) const;
    int nextMessageRowForGrouping(int row) const;
    bool continuesSenderGroup(int row) const;
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
    mutable QHash<QString, QString> m_sanitizedHtmlCache;
    mutable QHash<QString, QList<QPair<QString, QString>>> m_htmlMemberDeps;
    UserProfileResolver *m_profiles = nullptr;
    QString mentionNameFor(const QString &userId, bool ask) const;
    std::function<QString(const QString &)> m_inlineImageResolver;
    double inlineImageAspect(const QString &source) const;
    std::function<double(const QString &)> m_inlineImageAspect;
    mutable bool m_hasInlineEmoji = false;
    mutable QHash<QString, QVariantList> m_messageSegmentsCache;
    void forgetRenderedHtml(const QString &eventId);
    void clearRenderedHtml();

public:
    Q_INVOKABLE void toggleSpoilers(const QString &eventId);

private:
    QSet<QString> m_spoilersRevealed;
    QString m_selfUserId;
    QList<TimelineEvent> m_events;
    QString m_latestCallEventId;
    QHash<QString, int> m_threadReplyCounts;
    void rebuildThreadReplyIndex();
    QHash<QString, QString> m_senderAvatarIndex;
    QHash<QString, QString> m_senderNameIndex;
    void noteSenderProfile(const TimelineEvent &event);
    void rebuildSenderProfileIndex();
    QString m_typingText;

    bool m_searchActive = false;
    QString m_searchQuery;
    QStringList m_searchResults;
    int m_searchIndex = -1;

    QString m_mentionAccentColor;
    QString m_mentionLinkColor;
    QString m_codeBackgroundColor;
    bool m_underlineMentions = false;

    bool m_showRoomActivity = true;
    bool m_showMembershipEvents = true;
    bool m_showProfileChangeEvents = true;
    bool m_threadRepliesInline = false;
    QString m_threadFilterRootId;
    bool m_hideThreadReplies = false;
    int m_senderGroupWindowSeconds = kDefaultSenderGroupWindowSeconds;
};
