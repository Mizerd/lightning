#pragma once

#include "matrix/MatrixClient.h"

class SettingsManager;

// v0.4 scaffold: implements the MatrixClient interface by delegating to the
// Rust FFI in rust/. Currently reports backend identity/version through the
// FFI but does not talk to a homeserver — login/sync/send are stubbed so
// the UI shows an honest "backend present, not feature complete" state.
//
// Full login/sync/E2EE wiring is a follow-up task tracked in
// docs/roadmap.md. Only ever constructed when ENABLE_RUST_SDK_BACKEND is
// defined.
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

    // MatrixClient interface -------------------------------------------------
    // All operations are stubbed for the v0.4 scaffold. Sends emit
    // errorOccurred; queries return empty. This is honest — the client is
    // present but does not do the work yet.
    void login(const QString &homeserver,
               const QString &user,
               const QString &password) override;
    void logout() override;
    bool restoreSession() override;
    bool isLoggedIn() const override { return false; }
    QString currentUserId() const override { return {}; }
    QString homeserverUrl() const override { return {}; }

    void startSync() override;
    void stopSync() override;
    ConnectionState connectionState() const override { return Disconnected; }

    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }

    QString displayNameFor(const QString &, const QString &) const override { return {}; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }

    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }

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
    void refuseSend(const char *op);

    SettingsManager *m_settings;
};
