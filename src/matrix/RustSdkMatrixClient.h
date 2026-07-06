#pragma once

#include "matrix/MatrixClient.h"

#include <QHash>
#include <QPair>
#include <QTimer>
#include <QVariantList>

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

    // Smoke-only session sidecar used with LIGHTNING_TEST_PERSISTENT_STORE=1.
    // The file contains a Matrix access token. This wrapper never logs it and
    // never exposes it to QML; Rust reads/writes it directly.
    void setPersistentSessionFile(const QString &absolutePath);

    // Introspection for the smoke harness. Populated after ensure has
    // resolved a store path; empty otherwise.
    QString rustStorePath() const;
    bool    rustStorePathIsOverride() const;
    QString currentDeviceId() const;

    // Testing hook for persistent smoke mismatch recovery. Destroys the Rust
    // handle and deletes only the currently selected account's SDK store path.
    // It never touches cache.sqlite, QSettings, or SecretStore entries.
    bool resetRustStore();

    // Smoke-only restore path. Uses the configured persistent session sidecar,
    // not QSettings/SecretStore.
    bool restoreSessionFromFile(const QString &homeserver,
                                const QString &userIdForStore);

    // Encrypted-room test probe (v0.5.0-prep+6). Bypasses the C++
    // CryptoManager::supportsE2ee gate and calls
    // mx_rust_probe_encrypted_send. The Rust side still performs the
    // encryption via matrix-sdk (e2e-encryption + sqlite features);
    // C++ never handles ciphertext or keys. Reserved for the headless
    // smoke harness so E2EE can be verified before the UI gate is
    // flipped. Emits encryptedSendProbeResult(...) with a safe marker
    // and either an SDK event id or a non-secret failure message. Do
    // NOT wire this into QML — the interactive send path stays gated
    // on supportsE2ee.
    void probeEncryptedSend(const QString &roomId,
                            const QString &body,
                            const QString &marker);

    // Key-backup recovery (v0.5.0-prep+7). Wraps
    // mx_rust_recover_from_backup so the smoke harness can attempt to
    // restore room keys via matrix-sdk's server-side secret storage
    // WITHOUT the recovery key ever transiting a log or QML property.
    // Result flows through keyBackupResult(...). Do NOT expose to QML.
    void recoverFromBackup(const QString &recoveryKey);

    // v0.5.0-prep+11. Reload a room's recent timeline from matrix-sdk
    // (calls Room::messages). Timeline events dedupe by event_id in
    // handleTimelineEvent so this is safe to call at any time. Emits
    // roomTimelineReloaded(roomId, total, decrypted, undecryptable)
    // when the SDK finishes, or errorOccurred with a non-secret
    // message on failure.
    void reloadRoomTimeline(const QString &roomId, int limit = 30);

    // v0.5.0. SAS emoji verification (receive-first). Drives
    // mx_rust_accept_verification / confirm / mismatch / cancel.
    // Results flow through the verification* signals below. Only one
    // active flow at a time in the Rust bridge; calling accept on a
    // stale flow id returns a non-secret error via errorOccurred.
    void acceptVerification(const QString &flowId);
    void confirmVerification(const QString &flowId);
    void mismatchVerification(const QString &flowId);
    void cancelVerification(const QString &flowId);

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

Q_SIGNALS:
    // v0.5.0-prep+6. Fires exactly once per probeEncryptedSend call.
    // `ok`: true when matrix-sdk returned a real server event id.
    // `marker`: caller-supplied opaque identifier (never contains
    //           the probe body). Safe to log.
    // `serverEventId`: only meaningful when ok == true.
    // `message`: non-secret failure detail when ok == false; empty on
    //            success. May be truncated / one-lined by the caller.
    void encryptedSendProbeResult(const QString &roomId,
                                  const QString &marker,
                                  bool ok,
                                  const QString &serverEventId,
                                  const QString &message);

    // v0.5.0-prep+7. Fires once per recoverFromBackup call.
    // `state`: "attempted" (dispatch received) or "ok" / "failed" once
    //          matrix-sdk finishes. `message`: non-secret failure detail
    //          or empty. Never contains the recovery key or imported
    //          key material.
    void keyBackupResult(const QString &state, const QString &message);

    // v0.5.0-prep+11. Fires once per reloadRoomTimeline() call.
    void roomTimelineReloaded(const QString &roomId,
                              int totalEvents,
                              int decryptedEvents,
                              int undecryptableEvents);

    // v0.5.0. SAS emoji verification lifecycle.
    void verificationRequestReceived(const QString &flowId,
                                     const QString &otherUserId,
                                     const QString &otherDeviceId,
                                     bool isSelfVerification);
    void verificationSasReady(const QString &flowId,
                              const QVariantList &emojis,
                              const QVariantList &decimals);
    void verificationDone(const QString &flowId);
    void verificationCancelled(const QString &flowId, const QString &message);
    void verificationFailed(const QString &flowId, const QString &message);

private:
    struct PendingSend {
        QString roomId;
        QString localEventId;
    };

    struct PendingProbe {
        QString roomId;
        QString marker;
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
    void handleEncryptedSendOk(const QJsonObject &event);
    void handleEncryptedSendFailed(const QJsonObject &event);
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
    QString m_sessionFilePath;
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
    QHash<QString, PendingProbe> m_pendingProbes;
    quint64 m_txnCounter = 0;
};
