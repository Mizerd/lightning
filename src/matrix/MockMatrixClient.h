#pragma once

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QList>
#include <QSet>

class SettingsManager;

// Deterministic in-memory backend used by the `--mock` CLI flag. Emits
// signals via QTimer::singleShot for realism where async matters. Zero
// network activity.
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

    // v0.7: lets the mock restore/switch sessions from the persisted
    // account registry so account-switch lifecycle tests can run offline.
    void setSettings(SettingsManager *settings) { m_settings = settings; }

    // Development-only screenshot/demo dataset. When enabled, rooms()/timeline()
    // serve a richer, fully deterministic scene (fictional people, Spaces,
    // polished conversations, a poll, media placeholders, fixed timestamps)
    // instead of the compact shared test fixtures. Enabled ONLY by
    // AppController::beginScreenshotDemo (never by tests), so the fixtures the
    // mock tests assert on are unchanged. No network; no real stores.
    void setScreenshotDemoMode(bool on);
    bool screenshotDemoMode() const { return m_screenshotDemoMode; }

    // Development-only screenshot-demo multi-account support. Three fictional
    // accounts (Alex / Taylor / Nova) each own a full, deterministic
    // room/timeline scene. The active account's scene is the live working copy
    // (m_rooms/m_timelines); switching snapshots it back and loads the target,
    // so each account's local mutations (sends, reactions, votes, read/unread)
    // survive a round trip. Everything is in-memory; no store, no network.
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
    // Panel toggles: globally suppress the seeded typing indicators, and hide
    // all room-list unread badges. Read-time filters (no data mutation), so they
    // are fully reversible. Emit the relevant change signals.
    void setDemoTypingSuppressed(bool suppressed);
    void setDemoUnreadHidden(bool hidden);
    bool demoTypingSuppressed() const { return m_demoTypingSuppressed; }
    bool demoUnreadHidden() const { return m_demoHideUnread; }
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

    // Development-only screenshot-demo media bridge. When the demo scene is
    // active the mock serves bundled local fixtures through the SAME
    // MediaBridge → MediaImageProvider path the Rust backend uses, so image /
    // video-poster / GIF rows and avatars render as real pictures (no network,
    // no mxc fetch, no token). Off outside demo mode, so the shared mock
    // fixtures and every other backend behaviour are unchanged.
    bool supportsMediaBridge() const override { return m_screenshotDemoMode; }
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
    // v0.7 outgoing @-mentions: record the ids (and expanded body) for tests,
    // then forward to the existing non-mention behaviour.
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
    // v0.6.0: mock thread timelines so ThreadController and the thread UI
    // are testable without a homeserver. Mirrors the composite timeline-id
    // contract of the Rust backend (root first, replies in room order).
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
    // v0.6.1: thread attachment sending — mirrors the SDK thread path so
    // ThreadController's attachment queue is testable without a homeserver.
    bool supportsAttachmentSend() const override { return true; }
    quint64 sendThreadAttachment(const QString &roomId,
                                 const QString &rootEventId,
                                 const QString &localPath, const QString &mime,
                                 const QString &caption, int width, int height,
                                 bool animated) override;
    quint64 sendThreadAttachmentBytes(const QString &roomId,
                                      const QString &rootEventId,
                                      const QByteArray &bytes,
                                      const QString &filename,
                                      const QString &mime, int width,
                                      int height) override;
    int threadAttachmentCallsForTest() const { return m_threadAttachmentCalls; }
    // Make the NEXT thread attachment send fail (queue rejection) for tests.
    void failNextThreadAttachmentForTest() { m_failNextThreadAttachment = true; }
    // v0.6.0 checkpoint 5: deterministic thread list + follow state. The
    // subscription map is mock-local (a stand-in for MSC4306 server state).
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
    bool paginationFailed(const QString &roomId) const override
    { return m_paginationFailed.contains(roomId); }
    bool paginationFailureTransient(const QString &roomId) const override
    { return m_transientPaginationFailures.contains(roomId); }

    // Deterministic runtime-QML coverage for the Retry presentation. This
    // backend is test/demo-only and never performs network I/O.
    void failNextPaginationForTest(bool transient = false)
    { m_failNextPagination = true; m_nextPaginationFailureTransient = transient; }

    // v0.7 timeline-hydration test hooks — the mock mirror of the Rust SDK
    // diff surface so the QML gate/anchor tests can stage the exact live
    // sequence (small snapshot → async fill batches → in-place Set updates)
    // deterministically. Test/demo-only; no network I/O.
    void resetTimelineForTest(const QString &roomId,
                              const QList<TimelineEvent> &events,
                              int paginationPages);
    void changeEventAtForTest(const QString &roomId, int index,
                              const TimelineEvent &event);
    void appendEventForTest(const QString &roomId, const TimelineEvent &event);
    void setPaginationDelayForTest(int ms) { m_paginationDelayMs = ms; }
    void setPaginationChunkForTest(const QList<TimelineEvent> &chunk)
    { m_paginationChunkOverride = chunk; }
    // v0.7 startup-lifecycle hooks: hold the restoration state open long
    // enough to assert on it, or reject the next restore like an expired
    // session would.
    void setRestoreDelayForTest(int ms) { m_restoreDelayMs = ms; }
    void failNextRestoreForTest() { m_failNextRestore = true; }

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
    QString m_lastSentBody;                 // v0.7 mention-test recording
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
    int m_restoreDelayMs = 0;
    bool m_failNextRestore = false;

    quint64 m_eventCounter = 0;
    quint64 m_txnCounter = 0;

    // v0.6.0: the single open mock thread timeline (composite id), rebuilt
    // from the room timeline on open and kept in sync by sendThreadReply.
    QString m_openThreadTimelineId;
    void rebuildOpenThreadTimeline();
    // v0.6.0 checkpoint 5.
    QString m_openThreadListRoom;
    QHash<QString, bool> m_threadSubscriptions; // roomId+"\x1f"+rootId → followed
    int m_markThreadReadCalls = 0;
    QStringList m_decryptionRetryRooms;
    void emitThreadList(const QString &roomId);
    // v0.6.1: thread attachment sending.
    quint64 m_opCounter = 0;
    int m_threadAttachmentCalls = 0;
    bool m_failNextThreadAttachment = false;
    quint64 appendThreadAttachment(const QString &roomId,
                                   const QString &rootEventId,
                                   const QString &fileName,
                                   const QString &mime);
};
