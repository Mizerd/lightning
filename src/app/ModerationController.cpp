#include "app/ModerationController.h"

#include "matrix/MatrixClient.h"

#include <QCoreApplication>

ModerationController::ModerationController(QObject *parent)
    : QObject(parent)
{
}

void ModerationController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    onLoggedOut(); // reset all cached state for the new client
    if (m_client) {
        connect(m_client, &MatrixClient::ignoreUserFinished, this,
                &ModerationController::onIgnoreFinished);
        connect(m_client, &MatrixClient::ignoredUsersReceived, this,
                &ModerationController::onIgnoredUsersReceived);
        connect(m_client, &MatrixClient::ignoredUsersChanged, this,
                &ModerationController::onIgnoredUsersChanged);
        connect(m_client, &MatrixClient::reportMessageFinished, this,
                &ModerationController::onReportFinished);
        connect(m_client, &MatrixClient::initialSyncDoneChanged, this,
                &ModerationController::onInitialSyncDoneChanged);
        connect(m_client, &MatrixClient::loggedOut, this,
                &ModerationController::onLoggedOut);
    }
    Q_EMIT stateChanged();
}

bool ModerationController::supported() const
{
    return m_client && m_client->supportsIgnoredUsers();
}

bool ModerationController::reportSupported() const
{
    return m_client && m_client->supportsEventReporting();
}

QStringList ModerationController::ignoredUsers() const
{
    return m_ignoredOrdered;
}

void ModerationController::ignoreUser(const QString &userId)
{
    if (!supported() || m_ignoreOp != 0 || userId.isEmpty())
        return;
    const quint64 opId = m_client->setUserIgnored(userId, true);
    if (opId == 0) {
        Q_EMIT ignoreActionFinished(
            userId, true, false,
            describeCategory(QStringLiteral("network")));
        return;
    }
    m_ignoreOp = opId;
    Q_EMIT stateChanged();
}

void ModerationController::unignoreUser(const QString &userId)
{
    if (!supported() || m_ignoreOp != 0 || userId.isEmpty())
        return;
    const quint64 opId = m_client->setUserIgnored(userId, false);
    if (opId == 0) {
        Q_EMIT ignoreActionFinished(
            userId, false, false,
            describeCategory(QStringLiteral("network")));
        return;
    }
    m_ignoreOp = opId;
    Q_EMIT stateChanged();
}

void ModerationController::refreshIgnoredUsers()
{
    if (!supported() || m_listOp != 0)
        return;
    const quint64 opId = m_client->requestIgnoredUsers();
    if (opId != 0)
        m_listOp = opId;
}

void ModerationController::beginReport(const QString &roomId,
                                       const QString &eventId)
{
    if (!reportSupported() || roomId.isEmpty() || eventId.isEmpty()
        || m_reportOp != 0) {
        return;
    }
    m_reportRoomId = roomId;
    m_reportEventId = eventId;
    m_reportPromptActive = true;
    Q_EMIT reportPromptChanged();
}

void ModerationController::submitReport(const QString &reason)
{
    if (!m_reportPromptActive || !m_client)
        return;
    const quint64 opId =
        m_client->reportMessage(m_reportRoomId, m_reportEventId, reason);
    m_reportPromptActive = false;
    m_reportRoomId.clear();
    m_reportEventId.clear();
    Q_EMIT reportPromptChanged();
    if (opId == 0) {
        Q_EMIT reportFinished(false,
                              describeCategory(QStringLiteral("network")));
        return;
    }
    m_reportOp = opId;
    Q_EMIT stateChanged();
}

void ModerationController::cancelReport()
{
    if (!m_reportPromptActive)
        return;
    m_reportPromptActive = false;
    m_reportRoomId.clear();
    m_reportEventId.clear();
    Q_EMIT reportPromptChanged();
}

void ModerationController::onIgnoreFinished(quint64 opId,
                                            const QString &userId,
                                            bool ignored, bool ok,
                                            const QString &category)
{
    if (opId == 0 || opId != m_ignoreOp)
        return;
    m_ignoreOp = 0;
    if (ok) {
        // Reflect immediately; the authoritative push follows from sync
        // and re-applies the same truth.
        if (ignored) {
            if (!m_ignored.contains(userId)) {
                m_ignored.insert(userId);
                m_ignoredOrdered.append(userId);
            }
        } else {
            m_ignored.remove(userId);
            m_ignoredOrdered.removeAll(userId);
        }
        ++m_revision;
        Q_EMIT ignoreActionFinished(
            userId, ignored, true,
            ignored ? QCoreApplication::translate(
                          "ModerationController",
                          "%1 is now ignored. You will no longer see "
                          "their messages.")
                          .arg(userId)
                    : QCoreApplication::translate(
                          "ModerationController",
                          "%1 is no longer ignored. New messages will "
                          "appear again.")
                          .arg(userId));
    } else {
        Q_EMIT ignoreActionFinished(userId, ignored, false,
                                    describeCategory(category));
    }
    Q_EMIT stateChanged();
}

void ModerationController::onIgnoredUsersReceived(quint64 opId, bool ok,
                                                  const QStringList &users)
{
    if (opId == 0 || opId != m_listOp)
        return;
    m_listOp = 0;
    if (!ok)
        return; // keep the last known list; sync pushes will correct it
    applyIgnoredList(users);
}

void ModerationController::onIgnoredUsersChanged(const QStringList &users)
{
    // The sync push is authoritative for local AND remote changes.
    applyIgnoredList(users);
}

void ModerationController::onReportFinished(quint64 opId, const QString &,
                                            const QString &, bool ok,
                                            const QString &category)
{
    if (opId == 0 || opId != m_reportOp)
        return;
    m_reportOp = 0;
    if (ok) {
        Q_EMIT reportFinished(
            true, QCoreApplication::translate(
                      "ModerationController",
                      "The message was reported to the server "
                      "administrator."));
    } else {
        Q_EMIT reportFinished(false, describeCategory(category));
    }
    Q_EMIT stateChanged();
}

void ModerationController::onInitialSyncDoneChanged()
{
    // First authoritative read per session: the notification guard and the
    // menus need the set before the user opens Settings.
    if (m_client && m_client->initialSyncDone() && !m_initialListLoaded) {
        m_initialListLoaded = true;
        refreshIgnoredUsers();
    }
}

void ModerationController::onLoggedOut()
{
    m_ignored.clear();
    m_ignoredOrdered.clear();
    m_ignoreOp = 0;
    m_listOp = 0;
    m_reportOp = 0;
    m_initialListLoaded = false;
    ++m_revision;
    m_reportPromptActive = false;
    m_reportRoomId.clear();
    m_reportEventId.clear();
    Q_EMIT reportPromptChanged();
    Q_EMIT stateChanged();
}

void ModerationController::applyIgnoredList(const QStringList &users)
{
    QSet<QString> next(users.cbegin(), users.cend());
    if (next == m_ignored)
        return;
    m_ignored = next;
    m_ignoredOrdered = users;
    ++m_revision;
    Q_EMIT stateChanged();
}

QString ModerationController::describeCategory(const QString &category)
{
    if (category == QLatin1String("forbidden")) {
        return QCoreApplication::translate(
            "ModerationController", "The server refused this action.");
    }
    if (category == QLatin1String("not_found")) {
        return QCoreApplication::translate(
            "ModerationController",
            "That message no longer exists on the server.");
    }
    if (category == QLatin1String("rate_limited")) {
        return QCoreApplication::translate(
            "ModerationController",
            "The server is rate limiting this action. Try again shortly.");
    }
    return QCoreApplication::translate(
        "ModerationController", "Could not reach the server. Try again.");
}
