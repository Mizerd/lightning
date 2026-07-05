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

// Pure-C++ Matrix backend against the Client-Server r0/v3 HTTP API.
//
// v0.3 scope:
//   - v0.2 features (login, whoami, /sync, room list, timeline for
//     m.room.message text/notice/emote, send text, logout).
//   - Backfill via GET /rooms/{id}/messages?dir=b (pagination).
//   - Local echo resolution: PUT-response event_id replaces "local:<txn>" id.
//   - Sender display-name lookup from m.room.member state (per-room cache).
//   - Media receive: m.image / m.file (mxc:// resolved to HTTP URLs).
//   - Media send: POST /_matrix/media/v3/upload + send m.image / m.file.
//   - Replies (m.in_reply_to), edits (m.replace), redactions.
//   - Reactions (m.reaction / m.annotation) with toggle-off via redact.
//   - Typing: PUT /rooms/{id}/typing/{userId} with debounce.
//   - Read receipts: POST /rooms/{id}/receipt/m.read/{eventId}.
//   - Local SQLite cache (rooms + last N events per room + members).
//
// Still out of scope for v0.3:
//   - E2EE. Encrypted rooms remain read-only placeholders; sends are blocked.
//   - Sliding sync, SSO/OIDC, invites, spaces, threads, multi-account.
//   - Authenticated media endpoints (uses legacy unauthenticated /media/v3).
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
    // v0.4.4: real m.thread relation (MSC3440 / stable in v11). Overrides the
    // interface default (which fell back to sendReply) so the message is
    // delivered as a proper thread event rather than a plain in-reply-to.
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
    void loadCachedState();
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
    // v0.4.6: flips true after the first /sync response is fully parsed.
    // Reset on login/logout. Consumed via initialSyncDone() override so
    // QML can show "Loading rooms…" until the first response lands.
    bool m_initialSyncDone = false;

    QHash<QString, RoomInfo> m_rooms;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    QSet<QString> m_paginating;
    QHash<QString, QString> m_lastReceiptSent;  // roomId → last eventId we sent a receipt for

    // txn_id -> (roomId, currentEventId). currentEventId starts as
    // "local:<txn>" and is upgraded to the real event_id from the PUT
    // response, so /sync-side dedup still finds it.
    QHash<QString, QPair<QString, QString>> m_pendingSends;

    quint64 m_txnCounter = 0;
};
