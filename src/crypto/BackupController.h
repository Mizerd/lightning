#pragma once

#include <QObject>
#include <QString>

class MatrixClient;

// v0.9 (phase 9): key-backup and recovery MANAGEMENT — the write side that
// sits beside CryptoHealthModel's read-only health view. Every action is
// the SDK's own recovery/backup flow, dispatched by name and answered by
// op id; this class never touches key material except to hold a freshly
// minted recovery key in memory for the one-time display and drop it on
// dismiss.
//
// SECURITY: recoveryKey is real secret material while non-empty. It is
// never logged, never persisted, never placed in settings, and cleared by
// dismissRecoveryKey() (which the dialog calls on close) and on sign-out.
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
