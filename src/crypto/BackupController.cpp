#include "crypto/BackupController.h"

#include "matrix/MatrixClient.h"

BackupController::BackupController(QObject *parent)
    : QObject(parent)
{
}

void BackupController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    m_op = 0;
    m_error.clear();
    m_recoveryKey.clear();
    m_lastAction.clear();
    Q_EMIT stateChanged();
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::backupActionFinished, this,
            [this](quint64 opId, const QString &action, bool ok,
                   const QString &recoveryKey, const QString &category) {
        if (opId == 0 || opId != m_op)
            return;
        m_op = 0;
        m_lastAction = action;
        if (ok) {
            m_error.clear();
            // Held for display only; the dialog dismisses it.
            m_recoveryKey = recoveryKey;
        } else {
            m_recoveryKey.clear();
            m_error = category == QLatin1String("forbidden")
                ? tr("The server refused this backup change.")
                : tr("The backup change did not complete. Nothing was changed.");
        }
        Q_EMIT stateChanged();
        requestProgress();
    });
    connect(m_client, &MatrixClient::backupProgress, this,
            [this](const QString &backupState, const QString &uploadState,
                   qint64 backedUp, qint64 total) {
        m_backupState = backupState;
        m_uploadState = uploadState;
        m_backedUp = backedUp;
        m_total = total;
        Q_EMIT progressChanged();
    });
    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        // Key material must not outlive the session it was minted in.
        m_op = 0;
        m_recoveryKey.clear();
        m_error.clear();
        m_lastAction.clear();
        m_backupState.clear();
        m_uploadState.clear();
        m_backedUp = 0;
        m_total = 0;
        Q_EMIT stateChanged();
        Q_EMIT progressChanged();
    });
}

void BackupController::runAction(const QString &action)
{
    if (!m_client || busy())
        return;
    static const QStringList known = {
        QStringLiteral("enable"), QStringLiteral("create_backup"),
        QStringLiteral("reset_key"), QStringLiteral("disable_and_delete"),
        QStringLiteral("disable_recovery"),
    };
    if (!known.contains(action)) {
        m_error = tr("Unknown backup action.");
        Q_EMIT stateChanged();
        return;
    }
    m_recoveryKey.clear();
    const quint64 opId = m_client->backupAction(action);
    if (opId == 0) {
        m_error = tr("Backup management is not available on this backend.");
        Q_EMIT stateChanged();
        return;
    }
    m_op = opId;
    m_lastAction = action;
    m_error.clear();
    Q_EMIT stateChanged();
}

void BackupController::requestProgress()
{
    if (m_client)
        m_client->requestBackupProgress();
}

void BackupController::dismissRecoveryKey()
{
    if (m_recoveryKey.isEmpty())
        return;
    m_recoveryKey.clear();
    Q_EMIT stateChanged();
}
