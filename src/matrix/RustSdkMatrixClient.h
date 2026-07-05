#pragma once

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QPair>
#include <QTimer>

class SettingsManager;

// QObject/C++ wrapper for the Matrix Rust SDK bridge. QML and models still
// talk only to MatrixClient; Rust owns the SDK client, async work, and SDK
// SQLite store behind the C ABI.
class RustSdkMatrixClient final : public MatrixClient
{
    Q_OBJECT
public:
    explicit RustSdkMatrixClient(SettingsManager *settings, QObject *parent = nullptr);
    ~RustSdkMatrixClient() override;

    // Introspection surfaced for the Settings screen / logs.
    QString rustBackendName() const;
    QString rustBackendStatus() const;
    QString rustBackendVersion() const;
    bool    rustSupportsE2ee() const;

    // Testing hook. When non-empty, the SDK store is created at exactly
    // this absolute path, bypassing the per-account subdirectory layout
    // under matrix::app_data::primaryRoot(). Reserved for the headless
    // smoke harness so back-to-back password logins start from a clean
    // temporary crypto store and cannot hit the SDK's
    // "account in the store doesn't match the account in the
    // constructor" error. Must be called BEFORE login() / restoreSession().
    void setStorePathOverride(const QString &absolutePath);

    // Introspection for the smoke harness. Populated after ensure has
    // resolved a store path; empty otherwise.
    QString rustStorePath() const;
    bool    rustStorePathIsOverride() const;

    // MatrixClient interface -------------------------------------------------
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
    QUrl mediaThumbnailUrl(const QString &mxcUrl, int width, int height, bool crop) const override;

    void sendTextMessage(const QString &roomId, const QString &body) override;
    void sendReply(const QString &roomId,
                   const QString &replyToEventId,
                   const QString &body) override;
    void editMessage(const QString &roomId,
                     const QString &targetEventId,
                     const QString &newBody) override;
    void redactEvent(const QString &roomId,
                     const QString &eventId,
                     const QString &reason = QString()) override;
    void toggleReaction(const QString &roomId,
                        const QString &targetEventId,
                        const QString &key) override;
    void sendTyping(const QString &roomId, bool isTyping, int timeoutMs = 20000) override;
    void sendReadReceipt(const QString &roomId, const QString &eventId) override;
    void sendImage(const QString &roomId, const QString &localPath) override;
    void sendFile(const QString &roomId, const QString &localPath) override;

    void loadOlderMessages(const QString &roomId) override;
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

private:
    struct PendingSend {
        QString roomId;
        QString localEventId;
    };

    void refuseSend(const char *op);
    void setState(ConnectionState state);
    void setInitialSyncDone(bool done);
    void clearLocalState(bool clearPersisted);
    void ensurePollTimer();
    bool ensureRustHandleForUser(const QString &userIdForStore);
    QString rustStorePathForUser(const QString &userIdForStore) const;
    void pollRustEvents();
    void handleRustEvent(const QJsonObject &event);
    void handleRoomsEvent(const QJsonArray &rooms);
    void handleTimelineEvent(const QJsonObject &event);
    void handleSendOk(const QJsonObject &event);
    void handleSendFailed(const QJsonObject &event);
    QString nextTxnId();
    bool isRoomEncrypted(const QString &roomId) const;
    TimelineEvent buildOwnEcho(const QString &roomId,
                               const QString &body,
                               TimelineEvent::Type type) const;
    void failPendingSend(const QString &transactionId, const QString &message);

    SettingsManager *m_settings;
    void *m_rustHandle = nullptr;
    QString m_storePath;
    QString m_storePathOverride;
    QString m_homeserver;
    QString m_userId;
    QString m_deviceId;
    bool m_loggedIn = false;
    ConnectionState m_state = Disconnected;
    bool m_initialSyncDone = false;
    QTimer m_pollTimer;
    QHash<QString, RoomInfo> m_rooms;
    QHash<QString, QList<TimelineEvent>> m_timelines;
    QHash<QString, PendingSend> m_pendingSends;
    quint64 m_txnCounter = 0;
};
