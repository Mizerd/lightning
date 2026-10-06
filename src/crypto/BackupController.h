#pragma once

#include <QObject>
#include <QString>

class MatrixClient;

// Key-backup and recovery management, the write side beside CryptoHealthModel.
// Every action is the SDK's own recovery/backup flow, dispatched by name and
// answered by op id. The only key material held is a newly minted recovery
// key, kept in memory for its one-time display.
//
// Security: recoveryKey is a real secret while non-empty. Never logged,
// persisted or stored in settings; cleared by dismissRecoveryKey() and on
// sign-out. A CURRENT recovery key passed to runCrossSigning() is handed to
// the client at once and not kept.
class BackupController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString lastAction READ lastAction NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // Machine-readable reason of the last failure ("keys_unavailable",
    // "recovery_key_required", "cancelled", ...); empty when there is none.
    // QML branches on it, never on the translated `error` text.
    Q_PROPERTY(QString errorCategory READ errorCategory NOTIFY stateChanged)
    // Cross-signing setup/reset: the server wants the account password.
    Q_PROPERTY(bool passwordRequired READ passwordRequired NOTIFY stateChanged)
    // The last password answer was rejected.
    Q_PROPERTY(bool wrongPassword READ wrongPassword NOTIFY stateChanged)
    // OAuth (MAS) accounts approve the new keys in a browser at this URL;
    // empty otherwise. Only an https URL (http on loopback) is ever exposed.
    // The operation keeps polling while it is set.
    Q_PROPERTY(QString approvalUrl READ approvalUrl NOTIFY stateChanged)
    // Cancel was pressed and the backend has not confirmed yet. Still busy:
    // an approval that already went through completes the operation.
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY stateChanged)
    // A non-error note about the last result (an approval that won the race
    // against Cancel); empty otherwise.
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    // One-time: the new recovery key after "enable" / "reset_key" or a
    // cross-signing action that minted one. Shown, then dismissed.
    Q_PROPERTY(QString recoveryKey READ recoveryKey NOTIFY stateChanged)
    // Progress snapshot (requestProgress). backupState is the SDK's
    // BackupState in lowercase ("enabled", "creating", …); uploadState is
    // idle | uploading | done | error | unknown; counts are room keys.
    Q_PROPERTY(QString backupState READ backupState NOTIFY progressChanged)
    Q_PROPERTY(QString uploadState READ uploadState NOTIFY progressChanged)
    Q_PROPERTY(qint64 backedUp READ backedUp NOTIFY progressChanged)
    Q_PROPERTY(qint64 total READ total NOTIFY progressChanged)

public:
    explicit BackupController(QObject *parent = nullptr);
    void setClient(MatrixClient *client);

    bool busy() const { return m_op != 0; }
    QString lastAction() const { return m_lastAction; }
    QString error() const { return m_error; }
    QString errorCategory() const { return m_errorCategory; }
    bool passwordRequired() const { return m_passwordRequired; }
    bool wrongPassword() const { return m_wrongPassword; }
    QString approvalUrl() const { return m_approvalUrl; }
    bool cancelling() const { return m_cancelling; }
    QString notice() const { return m_notice; }
    QString recoveryKey() const { return m_recoveryKey; }
    QString backupState() const { return m_backupState; }
    QString uploadState() const { return m_uploadState; }
    qint64 backedUp() const { return m_backedUp; }
    qint64 total() const { return m_total; }

    // The only approval pages Lightning opens: https, or http on a loopback
    // host (a local test server). Anything else from the server is refused.
    static bool isSafeApprovalUrl(const QString &url);

    // "enable" | "create_backup" | "reset_key" | "disable_and_delete" |
    // "disable_recovery" | "setup_cross_signing" | "reset_cross_signing".
    // Refused (with `error`) for anything else or while an action is in flight.
    // The two cross-signing actions start without a recovery key and without
    // consent to replace one: runCrossSigning(action, "", false).
    Q_INVOKABLE void runAction(const QString &action);
    // "setup_cross_signing" creates the account's cross-signing identity only
    // when the server has none (it never replaces one: "keys_unavailable");
    // "reset_cross_signing" is the explicit last resort. When the account
    // already has secret storage the new keys must go into it:
    // `currentRecoveryKey` keeps the existing recovery key working, and
    // `replaceRecoveryKey` (only after the user confirmed it) replaces a key
    // they no longer have. Neither: the backend refuses with
    // "recovery_key_required" before changing anything. Both: refused here.
    Q_INVOKABLE void runCrossSigning(const QString &action,
                                     const QString &currentRecoveryKey,
                                     bool replaceRecoveryKey);
    // Answer the password challenge of a cross-signing action. The caller's
    // field must wipe itself right after the call.
    Q_INVOKABLE void submitPassword(const QString &password);
    // Ask the backend to abandon a cross-signing action waiting for a password
    // or an approval. Stays busy (`cancelling`) until the backend reports what
    // actually happened.
    Q_INVOKABLE void cancelAuth();
    Q_INVOKABLE void requestProgress();
    Q_INVOKABLE void dismissRecoveryKey();

Q_SIGNALS:
    void stateChanged();
    void progressChanged();
    /// A backup action finished, successfully or not; AppController re-queries
    /// crypto health. Without it the Sessions card keeps its login-time
    /// snapshot and still offers "Set up recovery and backup" after a
    /// successful setup, and pressing it again makes Recovery::enable() create
    /// a new secret store, silently invalidating the recovery key the user just
    /// saved.
    void cryptoHealthStale();

private:
    void clearAuth();
    // Cancel on the user's behalf (an auth step or approval page Lightning
    // will not use); `reason` names it once the backend confirms.
    void requestCancel(const QString &reason);
    QString failureText(const QString &category) const;
    MatrixClient *m_client = nullptr;
    quint64 m_op = 0;
    QString m_lastAction;
    QString m_error;
    QString m_errorCategory;
    bool m_passwordRequired = false;
    bool m_wrongPassword = false;
    QString m_approvalUrl;
    bool m_cancelling = false;
    QString m_cancelReason;
    QString m_notice;
    QString m_recoveryKey;
    QString m_backupState;
    QString m_uploadState;
    qint64 m_backedUp = 0;
    qint64 m_total = 0;
};
