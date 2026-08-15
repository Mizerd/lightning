#include "app/UiaController.h"

#include "matrix/MatrixClient.h"

#include <QCoreApplication>

UiaController::UiaController(QObject *parent)
    : QObject(parent)
{
}

void UiaController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    onLoggedOut(); // reset for the new client
    if (m_client) {
        connect(m_client, &MatrixClient::uiaRequired, this,
                &UiaController::onUiaRequired);
        connect(m_client, &MatrixClient::deviceDeleteFinished, this,
                &UiaController::onDeleteFinished);
        connect(m_client, &MatrixClient::oauthManagementUrlReceived, this,
                &UiaController::onManagementUrl);
        connect(m_client, &MatrixClient::loggedOut, this,
                &UiaController::onLoggedOut);
    }
    Q_EMIT stateChanged();
}

bool UiaController::supported() const
{
    return m_client && m_client->supportsDeviceDeletion();
}

void UiaController::signOutDevices(const QStringList &deviceIds,
                                   const QString &currentDeviceId)
{
    if (!supported() || busy() || m_challengeActive)
        return;
    QStringList ids;
    for (const QString &id : deviceIds) {
        // Guard, not policy: the server would allow deleting the current
        // device, but that is a logout with extra data-loss steps — the
        // UI's own Sign out flow (with its store cleanup) owns that.
        if (id.isEmpty() || id == currentDeviceId)
            continue;
        if (!ids.contains(id))
            ids.append(id);
    }
    if (ids.isEmpty())
        return;
    const quint64 opId = m_client->deleteDevices(ids);
    if (opId == 0) {
        Q_EMIT signOutFinished(
            false, describeCategory(QStringLiteral("network")));
        return;
    }
    m_deleteOp = opId;
    Q_EMIT stateChanged();
}

void UiaController::submitPassword(const QString &password)
{
    if (!m_client || !m_challengeActive || m_deleteOp == 0
        || password.isEmpty()) {
        return;
    }
    // The challenge closes optimistically here (a fresh uiaRequired
    // reopens it on a wrong password). The QString is the caller's; the
    // QML field wipes itself immediately after this call, and every layer
    // below scrubs its transit copy.
    const bool accepted = m_client->uiaSubmitPassword(m_deleteOp, password);
    if (!accepted) {
        // Stale or lost challenge: report honestly rather than hanging.
        m_deleteOp = 0;
        clearChallenge();
        Q_EMIT signOutFinished(
            false, describeCategory(QStringLiteral("network")));
        Q_EMIT stateChanged();
        return;
    }
    m_challengeActive = false;
    m_wrongPassword = false;
    Q_EMIT stateChanged();
}

void UiaController::cancel()
{
    if (!m_client || m_deleteOp == 0) {
        clearChallenge();
        Q_EMIT stateChanged();
        return;
    }
    m_client->uiaCancel(m_deleteOp);
    m_deleteOp = 0;
    clearChallenge();
    Q_EMIT signOutFinished(
        false,
        QCoreApplication::translate("UiaController",
                                    "Sign-out was cancelled."));
    Q_EMIT stateChanged();
}

void UiaController::requestManagementUrl(const QString &deviceId)
{
    if (!m_client || m_urlOp != 0)
        return;
    const quint64 opId = m_client->requestOAuthManagementUrl(deviceId);
    if (opId == 0)
        return;
    m_urlOp = opId;
    Q_EMIT stateChanged();
}

void UiaController::onUiaRequired(quint64 uiaId, bool hasPasswordStage,
                                  bool wrongPassword,
                                  const QStringList &stages)
{
    if (uiaId == 0 || uiaId != m_deleteOp)
        return; // a stale/foreign challenge must not open our prompt
    m_challengeActive = true;
    m_passwordStage = hasPasswordStage;
    m_wrongPassword = wrongPassword;
    m_stages = stages;
    Q_EMIT stateChanged();
}

void UiaController::onDeleteFinished(quint64 opId, bool ok,
                                     const QString &category)
{
    if (opId == 0 || opId != m_deleteOp)
        return;
    m_deleteOp = 0;
    clearChallenge();
    if (ok) {
        Q_EMIT signOutFinished(
            true, QCoreApplication::translate("UiaController",
                                              "The session was signed out."));
    } else {
        Q_EMIT signOutFinished(false, describeCategory(category));
    }
    Q_EMIT stateChanged();
}

void UiaController::onManagementUrl(quint64 opId, bool ok, const QString &url)
{
    if (opId == 0 || opId != m_urlOp)
        return;
    m_urlOp = 0;
    if (ok && !url.isEmpty())
        Q_EMIT managementUrlReady(url);
    else
        Q_EMIT signOutFinished(
            false,
            QCoreApplication::translate(
                "UiaController",
                "The account's session-management page could not be "
                "determined."));
    Q_EMIT stateChanged();
}

void UiaController::onLoggedOut()
{
    // A signed-out (or switched) session must never keep a challenge that
    // a later account could answer.
    m_deleteOp = 0;
    m_urlOp = 0;
    clearChallenge();
    Q_EMIT stateChanged();
}

void UiaController::clearChallenge()
{
    m_challengeActive = false;
    m_passwordStage = false;
    m_wrongPassword = false;
    m_stages.clear();
}

QString UiaController::describeCategory(const QString &category)
{
    if (category == QLatin1String("forbidden")) {
        return QCoreApplication::translate(
            "UiaController",
            "The server refused the sign-out. The password may be wrong, "
            "or the session may not be yours to remove.");
    }
    if (category == QLatin1String("not_found")) {
        // The device is already gone — the refreshed list is the truth.
        return QCoreApplication::translate(
            "UiaController", "That session no longer exists.");
    }
    if (category == QLatin1String("rate_limited")) {
        return QCoreApplication::translate(
            "UiaController",
            "The server is rate limiting this action. Try again shortly.");
    }
    return QCoreApplication::translate(
        "UiaController", "Could not reach the server. Try again.");
}
