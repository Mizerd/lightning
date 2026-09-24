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
// sign-out.
class BackupController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString lastAction READ lastAction NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // One-time: the new recovery key after "enable" / "reset_key". Shown,
    // then dismissed. Empty otherwise.
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
    QString recoveryKey() const { return m_recoveryKey; }
    QString backupState() const { return m_backupState; }
    QString uploadState() const { return m_uploadState; }
    qint64 backedUp() const { return m_backedUp; }
    qint64 total() const { return m_total; }

    // "enable" | "create_backup" | "reset_key" | "disable_and_delete" |
    // "disable_recovery". Refused (with `error`) for anything else or while
    // an action is in flight.
    Q_INVOKABLE void runAction(const QString &action);
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
    MatrixClient *m_client = nullptr;
    quint64 m_op = 0;
    QString m_lastAction;
    QString m_error;
    QString m_recoveryKey;
    QString m_backupState;
    QString m_uploadState;
    qint64 m_backedUp = 0;
    qint64 m_total = 0;
};
