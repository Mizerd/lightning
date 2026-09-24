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
public:
    explicit MockMatrixClient(QObject *parent = nullptr);

    void login(const QString &homeserver,
               const QString &user,
               const QString &password) override;
    void logout() override;
    bool restoreSession() override;
    bool detachSession() override;

    // Lets the mock restore/switch sessions from the persisted account registry
    // so account-switch tests run offline.
    void setSettings(SettingsManager *settings) { m_settings = settings; }

    // Development-only screenshot dataset: a richer, deterministic scene
    // (fictional people, Spaces, a poll, media placeholders, fixed timestamps)
    // instead of the shared test fixtures. Enabled only by
    // AppController::beginScreenshotDemo, never by tests.
    void setScreenshotDemoMode(bool on);
    bool screenshotDemoMode() const { return m_screenshotDemoMode; }

    // Screenshot-demo accounts (Alex / Taylor / Nova), each with a full
    // deterministic scene. The active scene is the live working copy; switching
    // snapshots it back so each account's local mutations survive a round trip.
    // In-memory only.
    QStringList demoAccountUserIds() const { return m_demoAccountOrder; }
    QString demoDefaultRoom(const QString &userId) const;
    // Swap the live dataset to `userId`'s scene. Called by login/restoreSession
    // (so the real account switcher works) and directly by tests. No-op outside
    // demo mode or for an unknown account.
    void activateDemoAccount(const QString &userId);
    QString activeDemoAccount() const { return m_activeDemoUser; }
    // Restore all three accounts (or one) to their deterministic initial state,
    // discarding local mutations. Re-emits roomsChanged/timelineReset for the
    // active account so the live UI rebuilds.
    void resetDemoData();
    void resetDemoAccount(const QString &userId);
    // The first thread-root event id in a room (for scenario "open thread"),
    // or empty if the room has no thread.
    QString demoThreadRoot(const QString &roomId) const;
    // Panel toggles: suppress seeded typing indicators and hide room-list
    // unread badges. Read-time filters, so fully reversible.
    void setDemoTypingSuppressed(bool suppressed);
    void setDemoUnreadHidden(bool hidden);
    bool demoTypingSuppressed() const { return m_demoTypingSuppressed; }
    bool demoUnreadHidden() const { return m_demoHideUnread; }

    // Development-only local interactions on the demo scene (poll voting,
    // invite accept/reject, mark unread). Each mutates the working set and
    // emits the signal the UI listens to. Gated on demo mode.
    bool supportsPolls() const override { return m_screenshotDemoMode; }
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

    // Screenshot-demo media bridge: bundled fixtures served through the same
    // MediaBridge -> MediaImageProvider path as the Rust backend, so demo
    // images, posters, GIFs and avatars render. Off outside demo mode.
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
    // Outgoing @-mentions: record the ids and expanded body for tests, then
    // forward to the non-mention path.
    void sendTextMessage(const QString &roomId, const QString &body,
                         const QStringList &mentionUserIds) override;
    void sendReply(const QString &roomId, const QString &replyToEventId,
                   const QString &body,
                   const QStringList &mentionUserIds) override;
    void editMessage(const QString &roomId, const QString &targetEventId,
                     const QString &newBody,
                     const QStringList &mentionUserIds) override;
    // Deterministic member snapshot for the mention suggestion model (built
    // from the seeded room members; emitted asynchronously by op id).
    quint64 requestRoomMembers(const QString &roomId) override;
    QString lastSentBodyForTest() const { return m_lastSentBody; }
    QStringList lastMentionIdsForTest() const { return m_lastMentionIds; }
    // Mock thread timelines so ThreadController and the thread UI are testable
    // offline. Follows the Rust backend's composite-id contract (root first,
    // replies in room order).
    bool supportsThreadTimelines() const override { return true; }
    void openThread(const QString &roomId, const QString &rootEventId) override;
    void closeThread() override;
    void sendThreadReplyTo(const QString &roomId,
                           const QString &threadRootEventId,
                           const QString &inReplyToEventId,
                           const QString &body) override;
    void sendThreadReplyTo(const QString &roomId,
                           const QString &threadRootEventId,
                           const QString &inReplyToEventId,
                           const QString &body,
                           const QStringList &mentionUserIds) override;
    // Thread attachment sending, mirroring the SDK thread path.
    bool supportsAttachmentSend() const override { return true; }
    quint64 sendThreadAttachment(const QString &roomId,
                                 const QString &rootEventId,
                                 const QString &localPath, const QString &mime,
                                 const QString &caption, int width, int height,
                                 bool animated, qint64 durationMs = 0) override;
    quint64 sendThreadAttachmentBytes(const QString &roomId,
                                      const QString &rootEventId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime, int width,
                                      int height) override;
    int threadAttachmentCallsForTest() const { return m_threadAttachmentCalls; }
    // Make the next thread attachment send fail (queue rejection).
    void failNextThreadAttachmentForTest() { m_failNextThreadAttachment = true; }
    // Deterministic thread list + follow state; the subscription map stands in
    // for MSC4306 server state.
    bool supportsThreadList() const override { return true; }
    void openThreadList(const QString &roomId) override;
    void closeThreadList() override;
    void paginateThreadList(const QString &roomId) override;
    void markThreadRead(const QString &roomId,
                        const QString &rootEventId) override
    {
        Q_UNUSED(roomId);
        Q_UNUSED(rootEventId);
        ++m_markThreadReadCalls;
    }
    int markThreadReadCallsForTest() const { return m_markThreadReadCalls; }
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

    // Deterministic coverage for the Retry presentation.
    void failNextPaginationForTest(bool transient = false)
    { m_failNextPagination = true; m_nextPaginationFailureTransient = transient; }

    // Timeline-hydration test hooks mirroring the Rust diff surface, so QML
    // tests can stage the live sequence (small snapshot, async fill batches,
    // in-place Sets) deterministically.
    void resetTimelineForTest(const QString &roomId,
                              const QList<TimelineEvent> &events,
                              int paginationPages);
    // Drive the wired call stack (AppController -> CallController ->
    // NotificationManager) from integration tests.
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
    // Startup-lifecycle hooks: hold restoration open long enough to assert on
    // it, or reject the next restore like an expired session.
    void setRestoreDelayForTest(int ms) { m_restoreDelayMs = ms; }
    void failNextRestoreForTest() { m_failNextRestore = true; }

    // Read-receipt avatar hooks: serve avatar bytes through the real
    // MediaBridge -> MediaImageProvider path without the demo scene, and
    // hydrate one member the way the Rust room_members merge does.
    void setSupportsMediaBridgeForTest(bool on)
    { m_mediaBridgeSupportedForTest = on; }
    void setAvatarBytesForTest(const QString &mxc, const QByteArray &bytes,
                               const QString &mime)
    { m_avatarBytesForTest.insert(mxc, { bytes, mime }); }
    void setRoomMemberForTest(const QString &roomId, const MemberInfo &member);

private:
    void seedMockData();
    void seedScreenshotDemoData();     // development-only rich demo scene
    bool m_screenshotDemoMode = false;

    // One fictional demo account's complete, deterministic scene.
    struct DemoAccount {
        QString userId;
        QString homeserver;
        QString displayName;
        QString avatarMxc;
        QString defaultRoomId;          // deterministic initial selected room
        QList<RoomInfo> rooms;
        QHash<QString, QList<TimelineEvent>> timelines;
        QHash<QString, int> paginationRemaining;
    };
    QHash<QString, DemoAccount> m_demoAccounts;   // keyed by full user id
    QStringList m_demoAccountOrder;               // deterministic switcher order
    QString m_activeDemoUser;                     // whose scene is live now
    bool m_demoTypingSuppressed = false;          // panel: hide typing dots
    bool m_demoHideUnread = false;                // panel: hide unread badges
    DemoAccount buildDemoAccountAlex();
    DemoAccount buildDemoAccountTaylor();
    DemoAccount buildDemoAccountNova();
    void loadDemoAccountIntoWorkingSet(const DemoAccount &acct);
    void snapshotWorkingSetToActiveDemoAccount();
    // Tag every image/video/sticker row with a media bridge key so the demo
    // media path (fetchMedia/fetchMxcThumbnail) can resolve it to a fixture.
    static void finalizeDemoMedia(DemoAccount &acct);
    // Resolve a demo media key or avatar mxc segment to bundled fixture bytes.
    static bool loadDemoFixture(const QString &key, QByteArray *bytes,
                                QString *mime);
    quint64 m_mediaOpCounter = 0;
    void setState(ConnectionState s);
    QString nextEventId();
    QString nextTxnId();
    TimelineEvent *findEvent(const QString &roomId, const QString &eventId);
    void ackAfter(int ms, const QString &roomId, const QString &eventId);

    SettingsManager *m_settings = nullptr; // not owned; may stay null
    QString m_lastSentBody;                 // mention-test recording
    QStringList m_lastMentionIds;
    bool m_loggedIn = false;
    ConnectionState m_state = Disconnected;
    QString m_userId;
    QString m_homeserver;

    QList<RoomInfo> m_rooms;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    QHash<QString, int> m_paginationRemaining; // roomId → mock pages left
    QSet<QString> m_paginating;                 // roomId currently paginating
    QSet<QString> m_paginationFailed;
    QSet<QString> m_transientPaginationFailures;
    bool m_failNextPagination = false;
    bool m_nextPaginationFailureTransient = false;
    int m_paginationDelayMs = 300;
    QList<TimelineEvent> m_paginationChunkOverride;
    /// Remaining pages to serve fully filtered; see
    /// setFilteredPaginationPagesForTest.
    int m_filteredPaginationPages = 0;
    /// Whether the last completed page was one of them, answering
    /// lastPaginationFullyFiltered() like the Rust backend.
    QSet<QString> m_lastPaginationFiltered;
    int m_restoreDelayMs = 0;
    bool m_failNextRestore = false;

    // Read-receipt-chip avatar hooks (see the public ForTest setters).
    bool m_mediaBridgeSupportedForTest = false;
    struct AvatarBytesFixture {
        QByteArray bytes;
        QString mime;
    };
    QHash<QString, AvatarBytesFixture> m_avatarBytesForTest; // mxc → bytes

    quint64 m_eventCounter = 0;
    quint64 m_txnCounter = 0;

    // The single open mock thread timeline (composite id), rebuilt from the
    // room timeline on open and kept in sync by sendThreadReply.
    QString m_openThreadTimelineId;
    void rebuildOpenThreadTimeline();
    QString m_openThreadListRoom;
    QHash<QString, bool> m_threadSubscriptions; // roomId+"\x1f"+rootId → followed
    int m_markThreadReadCalls = 0;
    QStringList m_decryptionRetryRooms;
    void emitThreadList(const QString &roomId);
    // Thread attachment sending.
    quint64 m_opCounter = 0;
    int m_threadAttachmentCalls = 0;
    bool m_failNextThreadAttachment = false;
    quint64 appendThreadAttachment(const QString &roomId,
                                   const QString &rootEventId,
                                   const QString &fileName,
                                   const QString &mime);

public:
    // ── Discover / Join, search, UIA, moderation, drafts. Knobs are plain
    // public members; completions go through QTimer::singleShot so op ids are
    // stored before answers arrive, as with the real backend. ──
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
    // Name colours: enough to exercise the manager's caching and dedup;
    // `nameColorFetches` counts requests.
    bool supportsNameColors() const override { return true; }
    void fetchNameColor(const QString &userId, quint64 opId) override;
    void setNameColor(const QString &value, quint64 opId) override;
    QHash<QString, QString> mockNameColors;
    int nameColorFetches = 0;
    bool nameColorsSupportedOnServer = true;
    quint64 roomWidgets(const QString &roomId, const QString &theme,
                        const QString &language) override;
    /// Ready payloads, not raw state: {id, creator, kind, name, url, refusal,
    /// discloses}. The derivation (templating, https and authority rules) is
    /// security logic that lives once, in Rust, with its own tests; a second
    /// copy here could pass while the real one failed.
    QVariantList mockWidgets;
    // Off by default so no fixture grows a Pinned tab; turned on by tests that
    // need the room-info strip to overflow.
    bool mockSupportsPinnedMessages = false;
    bool supportsPinnedMessages() const override
    { return mockSupportsPinnedMessages; }
    // Whether this account may write widget state; tests flip it to prove the
    // gate.
    bool mockWidgetsCanManage = true;
    // Every widget write the controller asked for: (roomId, widgetId, json).
    QList<std::tuple<QString, QString, QString>> widgetWrites;
    quint64 writeRoomWidget(const QString &roomId, const QString &widgetId,
                            const QString &contentJson) override;
    quint64 m_mockWidgetWriteOp = 0;
    QString lastWidgetTheme;
    QString lastWidgetLanguage;

    // Bridges (MSC2346). Ready payloads again: sanitising lives in Rust.
    // `bridgeReads` records (roomId, allowNetwork) so tests can prove the room
    // list never triggers a /state read and a room is asked once per session.
    bool mockSupportsRoomBridges = true;
    bool supportsRoomBridges() const override { return mockSupportsRoomBridges; }
    /// Per room: the list the bridge would answer. A missing entry answers an
    /// empty list, which is a fact, not an error.
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
    void clearSearchIndex() override;
    /// Mirrors localsearch::MIN_QUERY_CHARS (a Rust constant cannot be
    /// included); a different minimum would let a UI pass here and fail in
    /// production.
    static constexpr int kMockMinQueryChars = 3;
    /// Event ids the test has "redacted" from the index.
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

    // Knobs, read and written by tests directly.
    QVariantMap mockResolveResult;      // empty → ok=false, category invalid
    QVariantList mockPublicRooms;       // rows for one directory page
    QString mockPublicRoomsNextBatch;
    QString mockJoinFailCategory;       // non-empty → join fails with it
    QString mockKnockFailCategory;
    QVariantList mockSpaceChildren;
    QVariantList mockSearchResults;     // rows for one search page
    QString mockSearchNextBatch;
    QVariantMap lastSearchFilters;
    bool mockUiaRequired = false;       // first delete raises a challenge
    QString mockUiaPassword;            // accepted password when challenged
    QString mockDeleteFailCategory;     // non-empty → terminal failure
    QString mockManagementUrl;
    QString mockReportFailCategory;
    QStringList mockIgnoredUsers;
    quint64 lastDeleteOp = 0;           // pending UIA challenge id
    QStringList lastDeletedDevices;
    QStringList joinedTargets;          // every join target dispatched

    // Profiles by user id. An absent own-account row means "never set" and the
    // lookup reports the localpart; a cleared name is stored as an empty string
    // and stays empty.
    QHash<QString, QString> mockDisplayNames;
    QHash<QString, QString> mockAvatarUrls;
    // Non-empty: every display-name write fails with this as the server's
    // message. Sticky, so retry-after-failure is drivable.
    QString mockDisplayNameFailReason;
    // Fail with no message (timeout, or nothing usable), which the UI words
    // differently from a server message.
    bool mockDisplayNameFailSilently = false;
    int displayNameWrites = 0;          // dispatched writes (dedup checks)
};
