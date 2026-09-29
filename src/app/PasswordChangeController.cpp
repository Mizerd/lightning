#include "app/PasswordChangeController.h"

#include "matrix/MatrixClient.h"

#include <QLoggingCategory>

#include <algorithm>

// Outcomes and categories only; never a password or the server's text.
Q_LOGGING_CATEGORY(lcPasswordChange, "lightning.account.password")

PasswordChangeController::PasswordChangeController(QObject *parent)
    : QObject(parent)
{
}

void PasswordChangeController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    onLoggedOut(); // nothing of the previous client survives
    if (m_client) {
        connect(m_client, &MatrixClient::passwordChangeProbed, this,
                &PasswordChangeController::onProbed);
        connect(m_client, &MatrixClient::passwordChangeFinished, this,
                &PasswordChangeController::onFinished);
        connect(m_client, &MatrixClient::loggedOut, this,
                &PasswordChangeController::onLoggedOut);
        connect(m_client, &MatrixClient::loginSucceeded, this,
                &PasswordChangeController::onLoggedIn);
    }
}

void PasswordChangeController::setOAuthAccountCheck(std::function<bool()> check)
{
    m_oauthCheck = std::move(check);
    Q_EMIT stateChanged();
}

bool PasswordChangeController::available() const
{
    return m_client && m_client->supportsPasswordChange();
}

QString PasswordChangeController::mode() const
{
    // An OAuth (MAS) account has no password Lightning could send.
    if (m_oauthCheck && m_oauthCheck())
        return QStringLiteral("external");
    if (m_probeKnown && !m_canChange) {
        return m_managementUrl.isEmpty() ? QStringLiteral("unavailable")
                                         : QStringLiteral("external");
    }
    return QStringLiteral("form");
}

void PasswordChangeController::refresh()
{
    // The OAuth check may have changed with the active account.
    Q_EMIT stateChanged();
    m_probeWanted = true;
    if (!available() || m_probeOp != 0)
        return;
    const quint64 opId = m_client->probePasswordChange();
    if (opId != 0)
        m_probeOp = opId;
}

bool PasswordChangeController::changePassword(const QString &currentPassword,
                                              const QString &newPassword,
                                              const QString &confirmPassword,
                                              bool signOutOtherDevices)
{
    if (!available() || busy() || mode() != QLatin1String("form"))
        return false;
    // Refused here, before anything reaches the backend.
    const QString problem =
        localProblem(currentPassword, newPassword, confirmPassword);
    if (!problem.isEmpty()) {
        setResult(false, describe(problem, false));
        return false;
    }
    const quint64 opId = m_client->changePassword(currentPassword, newPassword,
                                                  signOutOtherDevices);
    qCInfo(lcPasswordChange) << "password change sent=" << (opId != 0)
                             << "sign_out_others=" << signOutOtherDevices;
    if (opId == 0) {
        setResult(false, describe(QStringLiteral("failed"), false));
        return false;
    }
    m_changeOp = opId;
    m_signOutRequested = signOutOtherDevices;
    m_resultMessage.clear();
    m_resultOk = false;
    Q_EMIT stateChanged();
    return true;
}

void PasswordChangeController::clearResult()
{
    if (m_resultMessage.isEmpty() && !m_resultOk)
        return;
    m_resultMessage.clear();
    m_resultOk = false;
    Q_EMIT stateChanged();
}

QString PasswordChangeController::localProblem(const QString &currentPassword,
                                               const QString &newPassword,
                                               const QString &confirmPassword)
{
    if (currentPassword.isEmpty() || newPassword.isEmpty())
        return QStringLiteral("empty");
    if (newPassword != confirmPassword)
        return QStringLiteral("mismatch");
    // Characters, not UTF-16 units, and without copying the password.
    const auto characters = std::count_if(
        newPassword.cbegin(), newPassword.cend(),
        [](QChar c) { return !c.isLowSurrogate(); });
    if (characters < kMinimumLength)
        return QStringLiteral("too_short");
    return {};
}

QString PasswordChangeController::describe(const QString &category,
                                           bool signedOutOthers)
{
    if (category.isEmpty()) {
        return signedOutOthers
            ? tr("Password changed. Your other devices were signed out.")
            : tr("Password changed.");
    }
    if (category == QLatin1String("empty"))
        return tr("Enter your current password and a new one.");
    if (category == QLatin1String("mismatch"))
        return tr("The new passwords do not match.");
    if (category == QLatin1String("too_short"))
        return tr("Use at least %1 characters.").arg(kMinimumLength);
    if (category == QLatin1String("wrong_password"))
        return tr("The server did not accept your current password.");
    if (category == QLatin1String("weak_password"))
        return tr("The server refused the new password as too weak. "
                  "Try a longer or more varied one.");
    if (category == QLatin1String("rate_limited"))
        return tr("Too many attempts. Wait a moment and try again.");
    if (category == QLatin1String("unsupported"))
        return tr("Your server does not allow changing the password here.");
    if (category == QLatin1String("network"))
        return tr("Could not reach the server. Try again.");
    return tr("The password could not be changed. Try again later.");
}

void PasswordChangeController::onProbed(quint64 opId, bool known,
                                        bool canChange,
                                        const QString &managementUrl)
{
    if (opId == 0 || opId != m_probeOp)
        return;
    m_probeOp = 0;
    m_probeKnown = known;
    m_canChange = canChange;
    m_managementUrl = managementUrl;
    qCInfo(lcPasswordChange) << "password change probe known=" << known
                             << "can_change=" << canChange
                             << "account_page=" << !managementUrl.isEmpty();
    Q_EMIT stateChanged();
}

void PasswordChangeController::onFinished(quint64 opId, bool ok,
                                          const QString &category)
{
    // A stale or foreign answer (after a sign-out, or another account's).
    if (opId == 0 || opId != m_changeOp)
        return;
    m_changeOp = 0;
    qCInfo(lcPasswordChange) << "password change result ok=" << ok
                             << "category=" << category
                             << "sign_out_others=" << m_signOutRequested;
    const QString message =
        describe(ok ? QString() : (category.isEmpty()
                                       ? QStringLiteral("failed")
                                       : category),
                 m_signOutRequested);
    m_signOutRequested = false;
    setResult(ok, message);
    Q_EMIT finished(ok, message);
}

void PasswordChangeController::onLoggedOut()
{
    // A later account must never receive this one's answer or its page.
    m_changeOp = 0;
    m_probeOp = 0;
    m_signOutRequested = false;
    m_probeKnown = false;
    m_canChange = true;
    m_managementUrl.clear();
    m_resultMessage.clear();
    m_resultOk = false;
    Q_EMIT stateChanged();
}

void PasswordChangeController::onLoggedIn()
{
    // A store reset ends a session without loggedOut; a new session can never
    // answer an op of the old one (the generation filter drops it), so a
    // change or probe still pending here would stay "busy" for ever.
    if (m_changeOp != 0 || m_probeOp != 0) {
        m_changeOp = 0;
        m_probeOp = 0;
        m_signOutRequested = false;
        Q_EMIT stateChanged();
    }
    if (m_probeWanted)
        refresh();
}

void PasswordChangeController::setResult(bool ok, const QString &message)
{
    m_resultOk = ok;
    m_resultMessage = message;
    Q_EMIT stateChanged();
}
