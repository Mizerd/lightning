#pragma once

#include "matrix/MatrixClient.h"

#include <tuple>
#include <utility>
#include <QHash>
#include <QList>
#include <QSet>

class SettingsManager;

// Deterministic in-memory backend for `--mock`. Emits via
// QTimer::singleShot where async matters. No network activity.
class MockMatrixClient : public MatrixClient
{
    Q_OBJECT
    Q_PROPERTY(bool unthreadedReadReceipts MEMBER m_unthreadedReadReceipts)
    Q_PROPERTY(bool simulatedNetworkRoute MEMBER m_simulatedNetworkRoute)
    Q_PROPERTY(int roomTextSends MEMBER m_roomTextSends)
    Q_PROPERTY(int roomAttachmentCalls MEMBER m_roomAttachmentCalls)
    Q_PROPERTY(int threadVoiceCalls MEMBER m_threadVoiceCalls)
public:
    explicit MockMatrixClient(QObject *parent = nullptr);

    void login(const QString &homeserver,
               const QString &user,
               const QString &password) override;
    void logout() override;
    bool restoreSession() override;
    bool detachSession() override;
    bool networkRouteActive() const override { return m_simulatedNetworkRoute; }

    // Lets the mock restore/switch sessions from the persisted account registry
    // so account-switch tests run offline.
    void setSettings(SettingsManager *settings) { m_settings = settings; }

    void setScreenshotDemoMode(bool on);
    bool screenshotDemoMode() const { return m_screenshotDemoMode; }

    QStringList demoAccountUserIds() const { return m_demoAccountOrder; }
    QString demoDefaultRoom(const QString &userId) const;
    void activateDemoAccount(const QString &userId);
    QString activeDemoAccount() const { return m_activeDemoUser; }
    void resetDemoData();
    void resetDemoAccount(const QString &userId);
    QString demoThreadRoot(const QString &roomId) const;
    void setDemoTypingSuppressed(bool suppressed);
    void setDemoUnreadHidden(bool hidden);
    bool demoTypingSuppressed() const { return m_demoTypingSuppressed; }
    bool demoUnreadHidden() const { return m_demoHideUnread; }

    bool supportsPolls() const override { return m_screenshotDemoMode; }
    bool supportsJoinByAddress() const override { return true; }
    void sendPollResponse(const QString &roomId, const QString &threadRootId,
                          const QString &pollStartEventId,
                          const QStringList &answerIds) override;
    void acceptInvite(const QString &roomId) override;
    void rejectInvite(const QString &roomId) override;
    void setRoomMarkedUnread(const QString &roomId, bool unread) override;
    // Unlike the HTTP backend the mock offers favourites (the fixture's own
    // state), which makes the Favourites section reachable in QML/demo runs.
    bool supportsRoomFavourites() const override { return true; }
    void setRoomFavourite(const QString &roomId, bool favourite) override;
    void setRoomLowPriority(const QString &roomId, bool lowPriority) override;
    bool isLoggedIn() const override { return m_loggedIn; }
    QString currentUserId() const override { return m_userId; }
    QString homeserverUrl() const override { return m_homeserver; }

    void startSync() override;
    void stopSync() override;
    ConnectionState connectionState() const override { return m_state; }

    QList<RoomInfo> rooms() const override;
    QList<TimelineEvent> timeline(const QString &roomId) const override;

    QString displayNameFor(const QString &roomId, const QString &userId) const override;
    QString avatarMxcFor(const QString &roomId, const QString &userId) const override;
    QStringList typingUsersFor(const QString &roomId) const override;

    QUrl mediaDownloadUrl(const QString &mxcUrl) const override;
    QUrl mediaThumbnailUrl(const QString &mxcUrl,
                           int width, int height, bool crop) const override;

    bool supportsMediaBridge() const override
    { return m_screenshotDemoMode || m_mediaBridgeSupportedForTest; }
    quint64 fetchMedia(const QString &mediaKey, int kind,
                       int timeoutClass = 0) override;
    quint64 fetchMxcThumbnail(const QString &mxc, int width, int height) override;

    void sendTextMessage(const QString &roomId, const QString &body) override;
    void sendReply(const QString &roomId,
                   const QString &replyToEventId,
                   const QString &body) override;
    void sendThreadReply(const QString &roomId,
                         const QString &threadRootEventId,
                         const QString &body) override;
    void sendTextMessage(const QString &roomId, const QString &body,
                         const QStringList &mentionUserIds) override;
    void sendReply(const QString &roomId, const QString &replyToEventId,
                   const QString &body,
                   const QStringList &mentionUserIds) override;
    void editMessage(const QString &roomId, const QString &targetEventId,
                     const QString &newBody,
                     const QStringList &mentionUserIds) override;
    quint64 requestRoomMembers(const QString &roomId) override;
    quint64 requestRoomOwnPermissions(const QString &roomId) override;
    void setOwnPermissionsForTest(const QString &roomId,
                                  const QVariantMap &facts);
    int ownPermissionRequestsForTest() const { return m_ownPermissionRequests; }
    void setRoomSuccessorForTest(const QString &roomId,
                                 const QString &successorRoomId);
    QString lastSentBodyForTest() const { return m_lastSentBody; }
    QStringList lastMentionIdsForTest() const { return m_lastMentionIds; }
    bool supportsThreadTimelines() const override { return true; }
    void openThread(const QString &roomId, const QString &rootEventId) override;
    void closeThread() override;
    void setThreadRepliesInLiveTimeline(bool inLive) override
    { m_threadRepliesInLive = inLive; }
    void setUnthreadedReadReceipts(bool on) override
    { m_unthreadedReadReceipts = on; }
    quint64 sendThreadVoiceMessage(const QString &roomId,
                                   const QString &rootEventId,
                                   const QString &localPath,
                                   const QString &mime, qint64 durationMs,
                                   const QList<int> &waveform,
                                   const QString &replyToEventId) override;

    bool threadRepliesInLiveTimeline() const { return m_threadRepliesInLive; }
    int threadTextSendsForTest() const { return m_threadTextSends; }
    QString lastThreadSendRootForTest() const { return m_lastThreadSendRoot; }
    QString lastThreadSendBodyForTest() const { return m_lastThreadSendBody; }
    QString lastThreadSendReplyToForTest() const
    { return m_lastThreadSendReplyTo; }
    // Event context view: opt-in so suites written against the history walk
    // keep their behaviour. The window is a slice of the room's own mock
    // timeline around the event (kContextRadius each side).
    bool supportsEventContext() const override { return m_eventContextEnabled; }
    void setEventContextEnabled(bool enabled) { m_eventContextEnabled = enabled; }
    void openEventContext(const QString &roomId, const QString &eventId) override;
    void closeEventContext() override;
    void paginateEventContext(const QString &roomId, const QString &eventId,
                              bool forward) override;
    int eventContextOpenCount() const { return m_eventContextOpens; }
    int eventContextCloseCount() const { return m_eventContextCloses; }
    int eventContextPaginateCount() const { return m_eventContextPaginates; }
    QString openEventContextTimelineId() const { return m_openContextTimelineId; }
    void setHoldEventContext(bool hold) { m_holdEventContext = hold; }
    void deliverHeldEventContext();
    static constexpr int kContextRadius = 2;
    void sendThreadReplyTo(const QString &roomId,
                           const QString &threadRootEventId,
                           const QString &inReplyToEventId,
                           const QString &body) override;
    void sendThreadReplyTo(const QString &roomId,
                           const QString &threadRootEventId,
                           const QString &inReplyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds) override;
    bool supportsAttachmentSend() const override { return true; }
    quint64 sendThreadAttachment(const QString &roomId,
                                 const QString &rootEventId,
                                 const QString &localPath, const QString &mime,
                                 const QString &caption, int width, int height,
                                 bool animated, qint64 durationMs,
                                 const QString &replyToEventId) override;
    quint64 sendThreadAttachmentBytes(const QString &roomId,
                                      const QString &rootEventId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime, int width,
                                      int height,
                                      const QString &replyToEventId) override;
    quint64 sendAttachment(const QString &roomId, const QString &localPath,
                           const QString &mime, const QString &caption,
                           int width, int height, bool animated,
                           qint64 durationMs,
                           const QString &replyToEventId) override;
    quint64 sendAttachmentBytes(const QString &roomId, const QByteArray &bytes,
                                const QString &filename, const QString &mime,
                                int width, int height,
                                const QString &replyToEventId) override;
    QString lastAttachmentReplyToForTest() const { return m_lastAttachmentReplyTo; }
    QString lastAttachmentCaptionForTest() const { return m_lastAttachmentCaption; }
    int threadAttachmentCallsForTest() const { return m_threadAttachmentCalls; }
    QByteArray lastThreadAttachmentBytesForTest() const { return m_lastThreadAttachmentBytes; }
    void failNextThreadAttachmentForTest() { m_failNextThreadAttachment = true; }
    bool supportsThreadList() const override { return true; }
    void openThreadList(const QString &roomId) override;
    void closeThreadList() override;
    void paginateThreadList(const QString &roomId) override;
    void markThreadRead(const QString &roomId, const QString &rootEventId,
                        quint64 opId) override
    {
        ++m_markThreadReadCalls;
        m_threadReadOpIds.append(opId);
        if (m_autoAcceptThreadReads) {
            setRoomThreadUnreadForTest(roomId, 0);
            Q_EMIT threadReadMarkerAdvanced(roomId, rootEventId, opId);
        }
    }
    void setRoomMembershipForTest(const QString &roomId,
                                  RoomInfo::Membership membership)
    {
        for (auto &r : m_rooms) {
            if (r.id == roomId)
                r.membership = membership;
        }
        Q_EMIT roomsChanged();
    }
    void setRoomThreadUnreadForTest(const QString &roomId, int kind)
    {
        for (auto &r : m_rooms) {
            if (r.id != roomId
                || (r.unreadThreadsOnly == (kind > 0)
                    && r.mentionInThreads == (kind == 2)))
                continue;
            r.unreadThreadsOnly = kind > 0;
            r.mentionInThreads = kind == 2;
            if (kind > 0 && r.unreadCount == 0)
                r.unreadCount = 1;
            Q_EMIT roomUpdated(roomId);
        }
    }
    void setAutoAcceptThreadReadsForTest(bool on) { m_autoAcceptThreadReads = on; }
    int markThreadReadCallsForTest() const { return m_markThreadReadCalls; }
    QList<quint64> threadReadOpIdsForTest() const { return m_threadReadOpIds; }
    void retryDecryption(const QString &roomId) override
    {
        m_decryptionRetryRooms.append(roomId);
    }
    QStringList decryptionRetryRoomsForTest() const
    { return m_decryptionRetryRooms; }
    void queryThreadSubscription(const QString &roomId,
                                 const QString &rootEventId) override;
    void setThreadSubscribed(const QString &roomId, const QString &rootEventId,
                             bool subscribed) override;
    void editMessage(const QString &roomId,
                     const QString &targetEventId,
                     const QString &newBody) override;
    void redactEvent(const QString &roomId,
                     const QString &eventId,
                     const QString &reason) override;
    void toggleReaction(const QString &roomId,
                        const QString &targetEventId,
                        const QString &key) override;
    void sendTyping(const QString &roomId, bool isTyping, int timeoutMs) override;
    void sendReadReceipt(const QString &roomId, const QString &eventId) override;
    void sendImage(const QString &roomId, const QString &localPath) override;
    void sendFile(const QString &roomId, const QString &localPath) override;

    void loadOlderMessages(const QString &roomId) override;
    bool canPaginate(const QString &roomId) const override;
    bool paginationReady(const QString &roomId) const override
    { return m_paginationRemaining.contains(roomId); }
    bool paginating(const QString &roomId) const override;
    bool lastPaginationFullyFiltered(const QString &roomId) const override
    { return m_lastPaginationFiltered.contains(roomId); }
    bool paginationFailed(const QString &roomId) const override
    { return m_paginationFailed.contains(roomId); }
    bool paginationFailureTransient(const QString &roomId) const override
    { return m_transientPaginationFailures.contains(roomId); }

    void failNextPaginationForTest(bool transient = false)
    { m_failNextPagination = true; m_nextPaginationFailureTransient = transient; }

    void resetTimelineForTest(const QString &roomId,
                              const QList<TimelineEvent> &events,
                              int paginationPages);
    void emitCallSignalForTest(const CallSignal &signal)
    {
        Q_EMIT callSignalReceived(signal);
    }
    void changeEventAtForTest(const QString &roomId, int index,
                              const TimelineEvent &event);
    void appendEventForTest(const QString &roomId, const TimelineEvent &event);
    void setPaginationDelayForTest(int ms) { m_paginationDelayMs = ms; }
    void setPaginationChunkForTest(const QList<TimelineEvent> &chunk)
    { m_paginationChunkOverride = chunk; }
    /// Serve the next `pages` back-paginations as fully filtered pages: the
    /// cursor advances, the timeline gets nothing, and the start is not
    /// reached. This is the shape of a room whose recent history is MatrixRTC
    /// membership churn, which `lightning_event_filter` drops; without it the
    /// mock could only produce pages that add rows.
    void setFilteredPaginationPagesForTest(int pages)
    { m_filteredPaginationPages = pages; }
    void setRestoreDelayForTest(int ms) { m_restoreDelayMs = ms; }
    void failNextRestoreForTest() { m_failRestoresLeft = 1; }
    void failRestoresForTest(int count) { m_failRestoresLeft = count; }
    int restoreCallsForTest() const { return m_restoreCalls; }

    // Read-receipt avatar hooks: serve avatar bytes through the real
    // MediaBridge -> MediaImageProvider path without the demo scene, and
    // hydrate one member the way the Rust room_members merge does.
    // Warm account adoption, scripted for app-level tests: when enabled, the
    // mock claims the capability and "adopts" by remembering the ticket's
    // handle and activating the account as restoreSession() would. It never
    // touches the handle (the scripted bridge owns it).
    void setLiveAdoptionForTest(bool on) { m_liveAdoptionForTest = on; }
    bool supportsLiveAdoption() const override { return m_liveAdoptionForTest; }
    bool adoptLiveSession(const lightning::accounts::LiveSessionTicket &ticket) override
    {
        if (!m_liveAdoptionForTest || !ticket.valid())
            return false;
        m_adoptedHandleForTest = ticket.handle;
        m_adoptedUserForTest = ticket.userId;
        return restoreSession();
    }
    void *adoptedHandleForTest() const { return m_adoptedHandleForTest; }
    QString adoptedUserForTest() const { return m_adoptedUserForTest; }
    void setSupportsMediaBridgeForTest(bool on)
    { m_mediaBridgeSupportedForTest = on; }
    void setAvatarBytesForTest(const QString &mxc, const QByteArray &bytes,
                               const QString &mime)
    { m_avatarBytesForTest.insert(mxc, { bytes, mime }); }
    void setRoomMemberForTest(const QString &roomId, const MemberInfo &member);

private:
    bool m_liveAdoptionForTest = false;
    void *m_adoptedHandleForTest = nullptr;
    QString m_adoptedUserForTest;
    void seedMockData();
    void seedScreenshotDemoData();
    bool m_screenshotDemoMode = false;
    bool m_threadRepliesInLive = false;
    bool m_unthreadedReadReceipts = false;
    bool m_simulatedNetworkRoute = false;
    int m_roomTextSends = 0;
    int m_roomAttachmentCalls = 0;
    int m_threadVoiceCalls = 0;
    int m_threadTextSends = 0;
    QString m_lastThreadSendRoot;
    QString m_lastThreadSendBody;
    QString m_lastThreadSendReplyTo;

    struct DemoAccount {
        QString userId;
        QString homeserver;
        QString displayName;
        QString avatarMxc;
        QString defaultRoomId;
        QList<RoomInfo> rooms;
        QHash<QString, QList<TimelineEvent>> timelines;
        QHash<QString, int> paginationRemaining;
    };
    QHash<QString, DemoAccount> m_demoAccounts;
    QStringList m_demoAccountOrder;
    QString m_activeDemoUser;
    bool m_demoTypingSuppressed = false;
    bool m_demoHideUnread = false;
    DemoAccount buildDemoAccountAlex();
    DemoAccount buildDemoAccountTaylor();
    DemoAccount buildDemoAccountNova();
    void loadDemoAccountIntoWorkingSet(const DemoAccount &acct);
    void snapshotWorkingSetToActiveDemoAccount();
    static void finalizeDemoMedia(DemoAccount &acct);
    static bool loadDemoFixture(const QString &key, QByteArray *bytes,
                                QString *mime);
    quint64 m_mediaOpCounter = 0;
    void setState(ConnectionState s);
    QString nextEventId();
    QString nextTxnId();
    TimelineEvent *findEvent(const QString &roomId, const QString &eventId);
    void ackAfter(int ms, const QString &roomId, const QString &eventId);

    SettingsManager *m_settings = nullptr;
    QString m_lastSentBody;
    QStringList m_lastMentionIds;
    bool m_loggedIn = false;
    ConnectionState m_state = Disconnected;
    QString m_userId;
    QString m_homeserver;

    QList<RoomInfo> m_rooms;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    QHash<QString, int> m_paginationRemaining; // roomId → mock pages left
    QSet<QString> m_paginating;
    QSet<QString> m_paginationFailed;
    QSet<QString> m_transientPaginationFailures;
    bool m_failNextPagination = false;
    bool m_nextPaginationFailureTransient = false;
    int m_paginationDelayMs = 300;
    QList<TimelineEvent> m_paginationChunkOverride;
    int m_filteredPaginationPages = 0;
    QSet<QString> m_lastPaginationFiltered;
    int m_restoreDelayMs = 0;
    int m_failRestoresLeft = 0;
    int m_restoreCalls = 0;

    bool m_mediaBridgeSupportedForTest = false;
    struct AvatarBytesFixture {
        QByteArray bytes;
        QString mime;
    };
    QHash<QString, AvatarBytesFixture> m_avatarBytesForTest;

    quint64 m_eventCounter = 0;
    quint64 m_txnCounter = 0;

    // The single open mock thread timeline (composite id), rebuilt from the
    // room timeline on open and kept in sync by sendThreadReply.
    QString m_openThreadTimelineId;
    bool m_eventContextEnabled = false;
    bool m_holdEventContext = false;
    QString m_openContextTimelineId;
    QString m_heldContextRoom;
    QString m_heldContextEvent;
    int m_eventContextOpens = 0;
    int m_eventContextCloses = 0;
    int m_eventContextPaginates = 0;
    void rebuildOpenThreadTimeline();
    QString m_openThreadListRoom;
    QHash<QString, bool> m_threadSubscriptions;
    int m_markThreadReadCalls = 0;
    QList<quint64> m_threadReadOpIds;
    bool m_autoAcceptThreadReads = true;
    QStringList m_decryptionRetryRooms;
    void emitThreadList(const QString &roomId);
    quint64 m_opCounter = 0;
    QHash<QString, QVariantMap> m_ownPermissionsForTest;
    int m_ownPermissionRequests = 0;
    int m_threadAttachmentCalls = 0;
    QByteArray m_lastThreadAttachmentBytes;
    bool m_failNextThreadAttachment = false;
    QString m_lastAttachmentReplyTo;
    QString m_lastAttachmentCaption;
    quint64 appendThreadAttachment(const QString &roomId,
                                   const QString &rootEventId,
                                   const QString &fileName,
                                   const QString &mime,
                                   const QString &replyToEventId);
    quint64 appendRoomAttachment(const QString &roomId,
                                 const QString &fileName,
                                 const QString &mime,
                                 const QString &replyToEventId);

public:
    bool supportsRoomDiscovery() const override { return true; }
    quint64 resolveRoomTarget(const QString &input) override;
    quint64 searchPublicRooms(const QString &query, const QString &server,
                              const QString &since, int limit) override;
    quint64 joinRoomByIdOrAlias(const QString &target,
                                const QStringList &via) override;
    quint64 knockRoom(const QString &target, const QStringList &via,
                      const QString &reason) override;
    quint64 cancelKnock(const QString &roomId) override;
    quint64 requestSpaceChildren(const QString &spaceId) override;
    bool supportsMessageSearch() const override { return true; }

    // ── Local search over the mock's own timelines ──────────────────────
    //
    // Not the real SQLite FTS5 index: a substring scan that matches its
    // observable contract (newest first, room-scoped or account-wide, a
    // three-character minimum reported as "too_short", redactions and unsent
    // echoes excluded), so the find-bar surfaces are testable without a
    // homeserver.
    bool supportsLocalSearch() const override { return true; }

    // Widgets. Serves whatever `mockWidgets` holds, so the list, consent sheet
    // and refusal rows are reachable without a homeserver.
    bool supportsWidgets() const override { return true; }
    bool supportsNameColors() const override { return true; }
    void fetchNameColor(const QString &userId, quint64 opId) override;
    void setNameColor(const QString &value, quint64 opId) override;
    QHash<QString, QString> mockNameColors;
    int nameColorFetches = 0;
    bool nameColorsSupportedOnServer = true;
    bool nameColorFetchFails = false;
    quint64 roomWidgets(const QString &roomId, const QString &theme,
                        const QString &language) override;
    QVariantList mockWidgets;
    bool mockSupportsPinnedMessages = false;
    bool supportsPinnedMessages() const override
    { return mockSupportsPinnedMessages; }
    QString mockMediaKeepState;
    QString mediaKeepState() const override { return mockMediaKeepState; }
    bool mockWidgetsCanManage = true;
    QList<std::tuple<QString, QString, QString>> widgetWrites;
    quint64 writeRoomWidget(const QString &roomId, const QString &widgetId,
                            const QString &contentJson) override;
    quint64 m_mockWidgetWriteOp = 0;
    QString lastWidgetTheme;
    QString lastWidgetLanguage;

    bool mockSupportsRoomBridges = true;
    bool supportsRoomBridges() const override { return mockSupportsRoomBridges; }
    QHash<QString, QVariantList> mockRoomBridges;
    QList<std::pair<QString, bool>> bridgeReads;
    quint64 roomBridges(const QString &roomId, bool allowNetwork) override;
    quint64 localSearch(const QString &query, const QString &roomId,
                        int limit, int offset) override;
    quint64 searchIndexStats() override;
    quint64 sweepSearchIndex() override;
    quint64 deepenSearchIndex(const QString &roomId) override;
    void forgetIndexedEvent(const QString &eventId) override;
    void forgetIndexedRoom(const QString &roomId) override;
    bool clearSearchIndex() override;
    quint64 clearStoredMedia() override;
    int mediaClears = 0;
    quint64 clearEventCache() override;
    quint64 requestHomeserverInfo() override;
    quint64 requestAccountDataListing(const QString &roomId) override;
    quint64 requestUserClients(const QString &userId) override;
    QStringList userClientRequests;
    int eventCacheClears = 0;
    int homeserverInfoRequests = 0;
    QStringList accountDataRequests;
    quint64 m_aboutOp = 9000;
    quint64 indexAllRooms(bool resumeOnly) override;
    quint64 pauseIndexAll() override;
    quint64 cancelIndexAll() override;
    quint64 requestIndexAllStatus() override;
    void setIndexAllHold(unsigned bits) override { indexAllHold = bits; }
    QList<bool> indexAllStarts;
    int indexAllPauses = 0;
    int indexAllCancels = 0;
    unsigned indexAllHold = 0;
    /// A run the mock considers interrupted and resumable.
    bool mockIndexAllPending = false;
    static constexpr int kMockMinQueryChars = 3;
    QSet<QString> forgottenIndexEvents;
    QStringList forgottenIndexRooms;
    int indexSweeps = 0;
    QStringList deepIndexedRooms;
    bool indexCleared = false;
    quint64 searchMessages(const QString &term, const QString &roomId,
                           const QString &nextBatch, int limit,
                           const QVariantMap &filters = {}) override;
    bool supportsDeviceDeletion() const override { return true; }
    quint64 deleteDevices(const QStringList &deviceIds) override;
    bool uiaSubmitPassword(quint64 uiaId, const QString &password) override;
    void uiaCancel(quint64 uiaId) override;
    quint64 requestOAuthManagementUrl(const QString &deviceId) override;
    bool supportsIgnoredUsers() const override { return true; }
    quint64 setUserIgnored(const QString &userId, bool ignored) override;
    quint64 requestIgnoredUsers() override;
    bool supportsPushRules() const override { return true; }
    quint64 requestPushRules() override;
    quint64 writePushRule(const QString &kind, const QString &key,
                          const QString &value) override;
    bool supportsEventReporting() const override { return true; }
    quint64 reportMessage(const QString &roomId, const QString &eventId,
                          const QString &reason) override;

    // ── Own profile. The mock answers both the write and the read-back the
    // account registry caches, so the surface is testable offline.
    //
    // Lookup answers "not_found" for anyone but the own account and seeded
    // rows: confirming arbitrary user ids would change what other mock-backed
    // surfaces believe about strangers.
    bool supportsOwnProfileEditing() const override { return true; }
    quint64 fetchUserProfile(const QString &userId) override;
    void setOwnDisplayName(const QString &name, quint64 opId) override;

    QVariantMap mockResolveResult;
    QVariantList mockPublicRooms;
    QString mockPublicRoomsNextBatch;
    QString mockJoinFailCategory;
    QString mockKnockFailCategory;
    QVariantList mockSpaceChildren;
    QVariantList mockSearchResults;
    QString mockSearchNextBatch;
    QVariantMap lastSearchFilters;
    bool mockUiaRequired = false;
    QString mockUiaPassword;
    QString mockDeleteFailCategory;
    QString mockManagementUrl;
    QString mockReportFailCategory;
    QStringList mockIgnoredUsers;
    // The mock account's push rules, in PushRulesSnapshot's shape; writes
    // follow the same command grammar as rust/src/pushrules.rs. A non-empty
    // mockPushRuleFailCategory fails every write with that category.
    QVariantMap mockPushRules;
    QString mockPushRuleFailCategory;
    quint64 lastDeleteOp = 0;
    QStringList lastDeletedDevices;
    QStringList joinedTargets;

    QHash<QString, QString> mockDisplayNames;
    QHash<QString, QString> mockAvatarUrls;
    QString mockDisplayNameFailReason;
    bool mockDisplayNameFailSilently = false;
    int displayNameWrites = 0;
};
