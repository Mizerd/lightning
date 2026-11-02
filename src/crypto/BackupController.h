#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class MatrixClient;

class BackupController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString lastAction READ lastAction NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString errorCategory READ errorCategory NOTIFY stateChanged)
    Q_PROPERTY(bool passwordRequired READ passwordRequired NOTIFY stateChanged)
    Q_PROPERTY(bool wrongPassword READ wrongPassword NOTIFY stateChanged)
    Q_PROPERTY(QString approvalUrl READ approvalUrl NOTIFY stateChanged)
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY stateChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY stateChanged)
    // One-time: the new recovery key after "enable" / "reset_key" or a
    // cross-signing action that minted one. Shown, then dismissed.
    Q_PROPERTY(QString recoveryKey READ recoveryKey NOTIFY stateChanged)
    Q_PROPERTY(QString backupState READ backupState NOTIFY progressChanged)
    Q_PROPERTY(QString uploadState READ uploadState NOTIFY progressChanged)
    Q_PROPERTY(qint64 backedUp READ backedUp NOTIFY progressChanged)
    Q_PROPERTY(qint64 total READ total NOTIFY progressChanged)
    Q_PROPERTY(bool progressAnswered READ progressAnswered NOTIFY progressChanged)

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
    bool progressAnswered() const { return m_progressAnswered; }

    // The only approval pages Lightning opens: https, or http on a loopback
    // host (a local test server). Anything else from the server is refused.
    static bool isSafeApprovalUrl(const QString &url);

    static QString recoveryFailureText(const QString &category,
                                       bool sessionVerified = false,
                                       bool becameVerified = false);

    Q_INVOKABLE void runAction(const QString &action);
    Q_INVOKABLE void runCrossSigning(const QString &action,
                                     const QString &currentRecoveryKey,
                                     bool replaceRecoveryKey);
    Q_INVOKABLE void submitPassword(const QString &password);
    Q_INVOKABLE void cancelAuth();
    Q_INVOKABLE void requestProgress();
    Q_INVOKABLE QVariantMap signOutKeyRisk(const QString &recoveryState,
                                           int backupOnServer,
                                           const QVariantList &devices) const;
    Q_INVOKABLE void dismissRecoveryKey();

Q_SIGNALS:
    void stateChanged();
    void progressChanged();
    void cryptoHealthStale();

private:
    void clearAuth();
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
    bool m_setupInterruptedPending = false;
    void showSetupInterrupted();
    QString m_backupState;
    QString m_uploadState;
    qint64 m_backedUp = 0;
    qint64 m_total = 0;
    bool m_progressAnswered = false;
    quint64 m_progressRequest = 0;
};
