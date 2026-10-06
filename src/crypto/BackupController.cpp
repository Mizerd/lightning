#include "crypto/BackupController.h"

#include "matrix/MatrixClient.h"

#include <QHostAddress>
#include <QUrl>

namespace {

bool isCrossSigningAction(const QString &action)
{
    return action == QLatin1String("setup_cross_signing")
        || action == QLatin1String("reset_cross_signing");
}

bool isCancelCategory(const QString &category)
{
    return category == QLatin1String("cancelled")
        || category == QLatin1String("cancelled_reset")
        || category == QLatin1String("cancelled_reset_unconfirmed");
}

} // namespace

BackupController::BackupController(QObject *parent)
    : QObject(parent)
{
}

bool BackupController::isSafeApprovalUrl(const QString &url)
{
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty() || !parsed.userInfo().isEmpty())
        return false;
    const QString scheme = parsed.scheme().toLower();
    if (scheme == QLatin1String("https"))
        return true;
    if (scheme != QLatin1String("http"))
        return false;
    // Plain http only for a server on this machine (development/test).
    const QString host = parsed.host().toLower();
    if (host == QLatin1String("localhost"))
        return true;
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
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
    m_errorCategory.clear();
    m_notice.clear();
    clearAuth();
    m_recoveryKey.clear();
    m_lastAction.clear();
    Q_EMIT stateChanged();
    if (!m_client)
        return;
    connect(m_client, &MatrixClient::uiaRequired, this,
            [this](quint64 uiaId, bool hasPasswordStage, bool wrongPassword,
                   const QStringList &) {
        // Only our own operation may open our prompt.
        if (uiaId == 0 || uiaId != m_op || m_cancelling)
            return;
        if (!hasPasswordStage) {
            // The server asked for something this client cannot render. Stop
            // rather than hang on a prompt that cannot be answered; the
            // backend reports what stopping left behind.
            requestCancel(QStringLiteral("unsupported_auth"));
            return;
        }
        m_passwordRequired = true;
        m_wrongPassword = wrongPassword;
        Q_EMIT stateChanged();
    });
    connect(m_client, &MatrixClient::crossSigningApprovalRequired, this,
            [this](quint64 opId, const QString &url) {
        if (opId == 0 || opId != m_op || m_cancelling)
            return;
        // The page is opened in the system browser: never anything but a
        // secure web page, whatever the server sent.
        if (!isSafeApprovalUrl(url)) {
            requestCancel(QStringLiteral("unsafe_approval_url"));
            return;
        }
        m_approvalUrl = url;
        Q_EMIT stateChanged();
    });
    connect(m_client, &MatrixClient::backupActionFinished, this,
            [this](quint64 opId, const QString &action, bool ok,
                   const QString &recoveryKey, const QString &category) {
        if (opId == 0 || opId != m_op)
            return;
        const bool cancelWasRequested = m_cancelling;
        const QString cancelReason = m_cancelReason;
        m_op = 0;
        clearAuth();
        m_lastAction = action;
        m_notice.clear();
        if (ok) {
            m_errorCategory.clear();
            m_error.clear();
            // Held for display only; the dialog dismisses it. A key minted
            // after Cancel was pressed is shown all the same: the change it
            // belongs to happened.
            m_recoveryKey = recoveryKey;
            if (cancelWasRequested) {
                m_notice = tr("The approval had already gone through before "
                              "Cancel took effect, so the change was completed.");
            }
        } else {
            m_recoveryKey.clear();
            m_errorCategory = !cancelReason.isEmpty() && isCancelCategory(category)
                ? cancelReason : category;
            m_error = failureText(category);
            if (!cancelReason.isEmpty() && isCancelCategory(category)) {
                const QString why = cancelReason == QLatin1String("unsafe_approval_url")
                    ? tr("The approval page the server sent is not a secure "
                         "(https) address, so Lightning did not open it and "
                         "stopped.")
                    : tr("The server asked for an authentication step Lightning "
                         "cannot do here. Use your account's own management "
                         "page for this change.");
                m_error = why + QLatin1Char(' ') + m_error;
            }
        }
        Q_EMIT stateChanged();
        requestProgress();
        // On any outcome: even a failure can change server state, and the stale
        // snapshot gates destructive buttons.
        Q_EMIT cryptoHealthStale();
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
        clearAuth();
        m_recoveryKey.clear();
        m_error.clear();
        m_errorCategory.clear();
        m_notice.clear();
        m_lastAction.clear();
        m_backupState.clear();
        m_uploadState.clear();
        m_backedUp = 0;
        m_total = 0;
        Q_EMIT stateChanged();
        Q_EMIT progressChanged();
    });
}

QString BackupController::failureText(const QString &category) const
{
    // Never claim "nothing was changed" unless the backend says so: a failed
    // `enable` may already have created the backup version, and a failed
    // `disable_and_delete` may already have disabled it locally.
    if (category == QLatin1String("keys_unavailable")) {
        // Honest about why, and about what Lightning will not do on its own
        // (CLAUDE.md section 6).
        return tr("This account already has a cross-signing identity, but this "
                  "session does not hold its keys and they could not be created "
                  "again without replacing that identity. Lightning will not "
                  "replace it automatically. Verify this session with another "
                  "session, or enter your recovery key; resetting is the last "
                  "resort.");
    }
    if (category == QLatin1String("recovery_key_required")) {
        return tr("This account already has a recovery key. Enter it so the new "
                  "keys are stored with it and it keeps working. If you no "
                  "longer have it, Lightning can replace it with a new one "
                  "instead. Nothing was changed yet.");
    }
    if (category == QLatin1String("recovery_key_wrong")) {
        return tr("That recovery key does not unlock your secret storage. "
                  "Nothing was changed; check it and try again.");
    }
    if (category == QLatin1String("cancelled")) {
        return tr("Cancelled. The server did not receive any new keys, so "
                  "nothing changed on your account.");
    }
    if (category == QLatin1String("cancelled_reset")) {
        return tr("Cancelled. The server never accepted the replacement keys, "
                  "so your account keeps its existing cross-signing identity. "
                  "The new keys this session had already prepared for the reset "
                  "were discarded; verify this session with another session or "
                  "with your recovery key.");
    }
    if (category == QLatin1String("cancelled_reset_unconfirmed")) {
        return tr("Cancelled. The server never accepted the replacement keys, "
                  "so your account keeps its existing cross-signing identity. "
                  "This session still holds the unused replacement keys until "
                  "it next checks your identity with the server, which it could "
                  "not do just now.");
    }
    // A reset that failed after it started has already replaced this
    // session's keys locally; the backend has put them back in line with the
    // server and says which way it went. Never "nothing was changed".
    if (category == QLatin1String("reset_failed")) {
        return tr("The reset did not go through: the server kept your existing "
                  "cross-signing identity. The replacement keys this session had "
                  "already prepared were discarded. Verify this session with "
                  "another session or with your recovery key, or try the reset "
                  "again.");
    }
    if (category == QLatin1String("reset_failed_unconfirmed")) {
        return tr("The reset did not complete, and Lightning could not confirm "
                  "with the server what it left behind. Check the state above "
                  "again in a moment.");
    }
    if (category == QLatin1String("reset_incomplete")) {
        return tr("Your cross-signing identity was replaced, but the reset did "
                  "not finish. Use \"Finish setting up\" to complete it.");
    }
    if (category == QLatin1String("reset_keys_lost")) {
        return tr("The server accepted your new cross-signing identity, but "
                  "this session lost its keys while the reset was waiting for "
                  "your approval, so nobody holds them and the new identity "
                  "cannot verify anything. Nothing is fixed by waiting: run "
                  "\"Reset\" again.");
    }
    if (category == QLatin1String("device_signature_failed")) {
        return tr("Cross-signing is set up on your account, but this session "
                  "could not be signed with it yet. Use \"Finish setting up\" "
                  "in a moment to sign it.");
    }
    if (category == QLatin1String("backup_without_recovery")) {
        return tr("Your account has a key backup but no secret storage, and "
                  "Lightning cannot store the new keys alongside a backup it "
                  "cannot open. Nothing was changed. If you have that backup's "
                  "recovery key, restore it first. If you don't, the last "
                  "resort is \"Delete backup\": messages whose keys exist only "
                  "in that backup then become unreadable on every new session, "
                  "for good. Set up cross-signing again after that.");
    }
    if (category == QLatin1String("cancel_unconfirmed")) {
        return tr("Lightning could not confirm whether the reset was stopped "
                  "before the server accepted it. Check the state above again "
                  "in a moment.");
    }
    if (category == QLatin1String("timed_out"))
        return tr("The approval was not given in time. The server did not "
                  "receive the new keys; try again.");
    if (category == QLatin1String("recovery_state_unknown")) {
        return tr("Lightning could not tell whether secret storage is set up "
                  "yet. Wait a moment and try again.");
    }
    if (category == QLatin1String("forbidden"))
        return tr("The server refused this backup change.");
    return tr("The backup change did not complete. Some of it may already have "
              "been applied — check the state above.");
}

void BackupController::runAction(const QString &action)
{
    if (!m_client || busy())
        return;
    if (isCrossSigningAction(action)) {
        runCrossSigning(action, QString(), false);
        return;
    }
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
    m_errorCategory.clear();
    m_notice.clear();
    clearAuth();
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

void BackupController::runCrossSigning(const QString &action,
                                       const QString &currentRecoveryKey,
                                       bool replaceRecoveryKey)
{
    if (!m_client || busy())
        return;
    if (!isCrossSigningAction(action)) {
        m_error = tr("Unknown backup action.");
        Q_EMIT stateChanged();
        return;
    }
    const bool haveKey = !currentRecoveryKey.trimmed().isEmpty();
    if (haveKey && replaceRecoveryKey) {
        m_error = tr("Use either your current recovery key or a new one, not "
                     "both.");
        Q_EMIT stateChanged();
        return;
    }
    m_recoveryKey.clear();
    m_errorCategory.clear();
    m_notice.clear();
    clearAuth();
    const quint64 opId = m_client->crossSigningAction(
        action, haveKey ? currentRecoveryKey : QString(), replaceRecoveryKey);
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

void BackupController::submitPassword(const QString &password)
{
    if (!m_client || m_op == 0 || !m_passwordRequired || m_cancelling
        || password.isEmpty())
        return;
    // Close optimistically; a wrong password reopens it via uiaRequired.
    const bool accepted = m_client->uiaSubmitPassword(m_op, password);
    if (!accepted) {
        // Nothing was retried. Release a job that may still be parked in the
        // backend (its late report is ignored) and fail here, since a backend
        // that refused the answer may not report at all.
        m_client->uiaCancel(m_op);
        m_op = 0;
        clearAuth();
        m_errorCategory = QStringLiteral("network");
        m_error = tr("The password could not be sent. Try again.");
        Q_EMIT stateChanged();
        return;
    }
    m_passwordRequired = false;
    m_wrongPassword = false;
    Q_EMIT stateChanged();
}

void BackupController::cancelAuth()
{
    if (!m_client || m_op == 0 || m_cancelling
        || (!m_passwordRequired && m_approvalUrl.isEmpty()))
        return;
    requestCancel(QString());
}

void BackupController::requestCancel(const QString &reason)
{
    if (!m_client || m_op == 0)
        return;
    m_client->uiaCancel(m_op);
    // Busy until the backend's own result: an approval may already have gone
    // through, and only the backend knows what the cancel left behind.
    m_cancelling = true;
    m_cancelReason = reason;
    m_passwordRequired = false;
    m_wrongPassword = false;
    m_approvalUrl.clear();
    Q_EMIT stateChanged();
}

void BackupController::clearAuth()
{
    m_passwordRequired = false;
    m_wrongPassword = false;
    m_approvalUrl.clear();
    m_cancelling = false;
    m_cancelReason.clear();
}
