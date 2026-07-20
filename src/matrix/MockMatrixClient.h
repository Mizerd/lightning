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

    void sendTextMessage(const QString &roomId, const QString &body) override;
    void sendReply(const QString &roomId,
                   const QString &replyToEventId,
                   const QString &body) override;
    void sendThreadReply(const QString &roomId,
                         const QString &threadRootEventId,
                         const QString &body) override;
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
    void setState(ConnectionState s);
    QString nextEventId();
    QString nextTxnId();
    TimelineEvent *findEvent(const QString &roomId, const QString &eventId);
    void ackAfter(int ms, const QString &roomId, const QString &eventId);

    SettingsManager *m_settings = nullptr; // not owned; may stay null
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
