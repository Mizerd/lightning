#include "crypto/BackupController.h"

#include "matrix/MatrixClient.h"
#include "crypto/SignOutKeyRisk.h"

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

}

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
        if (uiaId == 0 || uiaId != m_op || m_cancelling)
            return;
        if (!hasPasswordStage) {
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
        if (opId == 0 && !ok && category == QLatin1String("setup_interrupted")) {
            if (busy()) {
                m_setupInterruptedPending = true;
                return;
            }
            showSetupInterrupted();
            return;
        }
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
        Q_EMIT cryptoHealthStale();
        if (m_setupInterruptedPending)
            showSetupInterrupted();
    });
    connect(m_client, &MatrixClient::backupProgress, this,
            [this](const QString &backupState, const QString &uploadState,
                   qint64 backedUp, qint64 total, quint64 requestId) {
        if (requestId != m_progressRequest)
            return;
        m_backupState = backupState;
        m_uploadState = uploadState;
        m_backedUp = backedUp;
        m_total = total;
        m_progressAnswered = true;
        Q_EMIT progressChanged();
    });
    connect(m_client, &MatrixClient::loggedOut, this, [this] {
        // Key material must not outlive the session it was minted in.
        m_op = 0;
        m_setupInterruptedPending = false;
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
        m_progressAnswered = false;
        m_progressRequest = 0;
        Q_EMIT stateChanged();
        Q_EMIT progressChanged();
    });
}

QString BackupController::failureText(const QString &category) const
{
    if (category == QLatin1String("keys_unavailable")) {
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
    if (category == QLatin1String("setup_interrupted")) {
        return tr("Setting up recovery (or creating a new recovery key) was "
                  "interrupted, when Lightning closed or switched accounts, "
                  "before the new recovery key could be shown. Your account may "
                  "now have secret storage whose key you were never given. "
                  "Check the state above: if recovery reads as set up, create a "
                  "new recovery key so that you hold one that works.");
    }
    if (category == QLatin1String("forbidden"))
        return tr("The server refused this backup change.");
    return tr("The backup change did not complete. Some of it may already have "
              "been applied — check the state above.");
}

void BackupController::showSetupInterrupted()
{
    m_setupInterruptedPending = false;
    const QString text = failureText(QStringLiteral("setup_interrupted"));
    if (m_error.isEmpty()) {
        m_lastAction = QStringLiteral("enable");
        m_errorCategory = QStringLiteral("setup_interrupted");
        m_error = text;
    } else {
        m_notice = text;
    }
    Q_EMIT stateChanged();
    Q_EMIT cryptoHealthStale();
    if (m_client)
        m_client->acknowledgeInterruptedSetup();
}

QString BackupController::recoveryFailureText(const QString &category,
                                              bool sessionVerified,
                                              bool becameVerified)
{
    const bool afterImport = category == QLatin1String("network")
        || category == QLatin1String("server_refused")
        || category == QLatin1String("secret_storage_mismatch")
        || category == QLatin1String("backup_key_unreadable")
        || category == QLatin1String("failed");
    if (sessionVerified && afterImport) {
        QString reason;
        if (category == QLatin1String("network"))
            reason = tr("Lightning could not reach your homeserver. Try again.");
        else if (category == QLatin1String("server_refused"))
            reason = tr("your homeserver refused the request. Try again later.");
        else if (category == QLatin1String("secret_storage_mismatch"))
            reason = tr("the backup key in your secret storage does not match "
                        "the key backup on the server.");
        else if (category == QLatin1String("backup_key_unreadable"))
            reason = tr("the backup key could not be read from your secret "
                        "storage. Try again.");
        else
            reason = tr("try again.");
        return becameVerified
            ? tr("This session is now verified, but the key backup could not "
                 "be restored: %1").arg(reason)
            : tr("This session is verified, but the key backup could not be "
                 "restored: %1").arg(reason);
    }
    if (category == QLatin1String("recovery_key_wrong")) {
        return tr("That recovery key or passphrase does not unlock this "
                  "account's secret storage. Check it and try again.");
    }
    if (category == QLatin1String("recovery_key_malformed")) {
        return tr("That is not a valid recovery key. Check it for a mistyped or "
                  "missing character. A passphrase works only if one was chosen "
                  "when recovery was set up.");
    }
    if (category == QLatin1String("no_secret_storage")) {
        return tr("This account has no secret storage on the server, so there "
                  "is nothing a recovery key can unlock. Verify this session "
                  "with another session instead.");
    }
    if (category == QLatin1String("secret_storage_unsupported")) {
        return tr("This account's secret storage uses a format Lightning cannot "
                  "open. Verify this session with another session instead.");
    }
    if (category == QLatin1String("secret_storage_mismatch")) {
        return tr("The recovery key was accepted, but what it unlocks no longer "
                  "matches this account's keys: they were replaced after this "
                  "recovery key was saved. Verify this session with another "
                  "session instead.");
    }
    if (category == QLatin1String("backup_key_unreadable")) {
        return tr("The key backup's key could not be read from your secret "
                  "storage. Check your connection and try again.");
    }
    if (category == QLatin1String("network")) {
        return tr("Lightning could not reach your homeserver. Check your "
                  "connection and try again.");
    }
    if (category == QLatin1String("server_refused")) {
        return tr("Your homeserver refused the request. Try again later.");
    }
    return tr("Recovery did not complete. Try again.");
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
    if (m_client) {
        if (m_progressAnswered) {
            m_progressAnswered = false;
            Q_EMIT progressChanged();
        }
        m_progressRequest = m_client->requestBackupProgress();
    }
}

QVariantMap BackupController::signOutKeyRisk(const QString &recoveryState,
                                             int backupOnServer,
                                             const QVariantList &devices) const
{
    const auto a = SignOutKeyRisk::assess(m_backupState, m_uploadState,
                                          m_backedUp, m_total, backupOnServer, recoveryState,
                                          m_progressAnswered, devices);
    return {
        { QStringLiteral("level"), a.name },
        { QStringLiteral("warns"), a.warns() },
        { QStringLiteral("pendingKeys"), a.pendingKeys },
        { QStringLiteral("totalKeys"), a.totalKeys },
        { QStringLiteral("otherVerifiedDevices"), a.otherVerifiedDevices },
    };
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
    const bool accepted = m_client->uiaSubmitPassword(m_op, password);
    if (!accepted) {
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
