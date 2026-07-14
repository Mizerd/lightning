#pragma once

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QList>
#include <QSet>

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

    // Deterministic runtime-QML coverage for the Retry presentation. This
    // backend is test/demo-only and never performs network I/O.
    void failNextPaginationForTest() { m_failNextPagination = true; }

private:
    void seedMockData();
    void setState(ConnectionState s);
    QString nextEventId();
    QString nextTxnId();
    TimelineEvent *findEvent(const QString &roomId, const QString &eventId);
    void ackAfter(int ms, const QString &roomId, const QString &eventId);

    bool m_loggedIn = false;
    ConnectionState m_state = Disconnected;
    QString m_userId;
    QString m_homeserver;

    QList<RoomInfo> m_rooms;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    QHash<QString, int> m_paginationRemaining; // roomId → mock pages left
    QSet<QString> m_paginating;                 // roomId currently paginating
    QSet<QString> m_paginationFailed;
    bool m_failNextPagination = false;

    quint64 m_eventCounter = 0;
    quint64 m_txnCounter = 0;
};
