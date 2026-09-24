#pragma once

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <memory>

class QNetworkReply;
class SettingsManager;
class CacheStore;
class QJsonObject;

// Experimental pure-C++ backend against the Client-Server HTTP API: login,
// /sync, room list and timeline, backfill, local echo resolution, per-room
// member names, media send/receive, replies, edits, redactions, reactions,
// typing, read receipts, Spaces, thread replies, and a local SQLite cache.
//
// Not authoritative for modern Matrix: no E2EE (encrypted rooms are
// read-only placeholders), no sliding sync, and legacy unauthenticated media
// URLs.
class CppHttpMatrixClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit CppHttpMatrixClient(SettingsManager *settings, QObject *parent = nullptr);
    ~CppHttpMatrixClient() override;

    void login(const QString &homeserver,
               const QString &user,
               const QString &password) override;
    void logout() override;
    bool restoreSession() override;
    bool detachSession() override;
    bool isLoggedIn() const override { return m_loggedIn; }
    QString currentUserId() const override { return m_userId; }
    QString homeserverUrl() const override { return m_homeserver; }

    void startSync() override;
    void stopSync() override;
    ConnectionState connectionState() const override { return m_state; }
    bool initialSyncDone() const override { return m_initialSyncDone; }

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
    // Real m.thread relation, delivered as a thread event rather than a plain
    // in-reply-to.
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
    bool paginating(const QString &roomId) const override;

private:
    // Session lifecycle helpers.
    void doWhoami();
    int loadCachedState();
    void openCacheFor(const QString &userId);
    void closeAndClearCache();
    void clearLocalSession(bool clearPersisted);

    // Sync loop.
    void startNextSync();
    void handleSyncResponse(const QJsonObject &syncObj);
    void processJoinedRooms(const QJsonObject &joined);
    void processStateEvent(RoomInfo &room, const QJsonObject &stateEvent);
    void processTimelineEvent(const QString &roomId, const QJsonObject &evObj);
    void processEphemeral(const QString &roomId, const QJsonObject &ephemeral);

    // Message helpers.
    void putSendJson(const QString &roomId,
                     const QString &type,           // "m.room.message" or "m.reaction"
                     const QJsonObject &content,
                     const QString &echoEventId,    // empty → no local-echo tracking
                     const QString &debugLabel);
    void redactByHttp(const QString &roomId,
                      const QString &eventId,
                      const QString &reason);
    TimelineEvent buildOwnEcho(const QString &roomId,
                               const QString &body,
                               TimelineEvent::Type type) const;

    // Utilities.
    void applyBearer(QNetworkRequest &request) const;
    void setState(ConnectionState);
    QUrl endpoint(const QString &path) const;
    QUrl mediaEndpoint(const QString &path) const;
    QString nextTxnId();
    bool isRoomEncrypted(const QString &roomId) const;
    void applyReactionEvent(const QString &roomId,
                            const QString &targetEventId,
                            const QString &key,
                            const QString &sender,
                            const QString &reactionEventId);
    void applyRedaction(const QString &roomId,
                        const QString &redactedEventId);
    void applyEdit(const QString &roomId,
                   const QString &targetEventId,
                   const QString &newBody);
    QString cachedDisplayName(const RoomInfo &room, const QString &userId) const;
    void enrichReplyPreview(TimelineEvent &e) const;

    SettingsManager *m_settings = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    std::unique_ptr<CacheStore> m_cache;

    QString m_homeserver;
    QString m_userId;
    QString m_deviceId;
    QString m_accessToken;
    QString m_syncToken;

    bool m_loggedIn = false;
    ConnectionState m_state = Disconnected;

    bool m_syncActive = false;
    QPointer<QNetworkReply> m_syncReply;
    QTimer m_syncRetryTimer;
    int m_syncBackoffMs = 5000;
    // True after the first /sync response is parsed; reset on login/logout.
    // Lets QML show "Loading rooms…" until then.
    bool m_initialSyncDone = false;

    QHash<QString, RoomInfo> m_rooms;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    QSet<QString> m_paginating;
    QHash<QString, QString> m_lastReceiptSent;  // roomId → last eventId we sent a receipt for

    // txn_id -> (roomId, currentEventId). currentEventId starts as
    // "local:<txn>" and becomes the real event_id from the PUT response, so
    // /sync dedup finds it.
    QHash<QString, QPair<QString, QString>> m_pendingSends;

    quint64 m_txnCounter = 0;
};
