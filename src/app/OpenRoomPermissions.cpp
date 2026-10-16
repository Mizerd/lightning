#include "app/OpenRoomPermissions.h"

#include "matrix/MatrixClient.h"

#include <QCoreApplication>

namespace {
// Unknown membership is not "not joined": nothing has said so yet.
bool knownNotJoined(const QString &ownMembership)
{
    return !ownMembership.isEmpty()
        && ownMembership != QLatin1String("joined");
}
} // namespace

OpenRoomPermissions::OpenRoomPermissions(QObject *parent) : QObject(parent) {}

void OpenRoomPermissions::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    // Another client is another account's answers.
    m_op = 0;
    clearFacts();
    if (m_client) {
        connect(m_client, &MatrixClient::roomOwnPermissionsReceived, this,
                &OpenRoomPermissions::onReceived);
        // Membership and power-level changes both arrive on this poke.
        connect(m_client, &MatrixClient::roomMemberEventSeen, this,
                &OpenRoomPermissions::onRoomPoked);
        connect(m_client, &MatrixClient::loggedOut, this,
                [this]() { setRoomId(QString()); });
    }
    request();
    Q_EMIT changed();
}

void OpenRoomPermissions::setRoomId(const QString &roomId)
{
    if (m_roomId == roomId)
        return;
    m_roomId = roomId;
    // Never show one room's answer in another, not even until the next one
    // lands.
    m_op = 0;
    clearFacts();
    request();
    Q_EMIT changed();
}

bool OpenRoomPermissions::sendKnown() const
{
    return knownNotJoined(m_ownMembership) || m_sendAnswered;
}

bool OpenRoomPermissions::canSendMessages() const
{
    return !knownNotJoined(m_ownMembership)
        && (!m_sendAnswered || m_canSendMessages);
}

QString OpenRoomPermissions::sendBlockedReason() const
{
    // RoomInfoController's own sentences, under its context, so the composer
    // says the same words whichever answer it read and the catalogues gain
    // nothing to translate twice.
    if (knownNotJoined(m_ownMembership))
        return QCoreApplication::translate("RoomInfoController",
                                           "You are not in this room.");
    if (m_sendAnswered && !m_canSendMessages)
        return QCoreApplication::translate(
            "RoomInfoController",
            "You do not have permission to post in this room.");
    return {};
}

void OpenRoomPermissions::request()
{
    if (!m_client || m_roomId.isEmpty())
        return;
    // A synchronous refusal (0) keeps the previous answer for this room.
    const quint64 opId = m_client->requestRoomOwnPermissions(m_roomId);
    if (opId != 0)
        m_op = opId;
}

void OpenRoomPermissions::clearFacts()
{
    m_ownMembership.clear();
    m_sendAnswered = false;
    m_canSendMessages = true;
    m_redactKnown = false;
    m_canRedactOthers = false;
}

void OpenRoomPermissions::onReceived(quint64 opId, const QString &roomId,
                                     const QVariantMap &facts)
{
    if (opId == 0 || opId != m_op || roomId != m_roomId)
        return; // another room, or a request a newer one superseded
    m_op = 0;
    // An absent key is unknown, never a refusal or a grant.
    const QString sendKey = QStringLiteral("canSendMessages");
    const QString redactKey = QStringLiteral("canRedactOthers");
    const QString membership =
        facts.value(QStringLiteral("ownMembership")).toString();
    const bool sendAnswered = facts.contains(sendKey);
    const bool canSend = !sendAnswered || facts.value(sendKey).toBool();
    const bool redactKnown = facts.contains(redactKey);
    const bool canRedact = redactKnown && facts.value(redactKey).toBool();
    // Every timeline row binds canRedactOthers: an answer that changes
    // nothing (most pokes) must not make them all re-evaluate.
    if (membership == m_ownMembership && sendAnswered == m_sendAnswered
        && canSend == m_canSendMessages && redactKnown == m_redactKnown
        && canRedact == m_canRedactOthers) {
        return;
    }
    m_ownMembership = membership;
    m_sendAnswered = sendAnswered;
    m_canSendMessages = canSend;
    m_redactKnown = redactKnown;
    m_canRedactOthers = canRedact;
    Q_EMIT changed();
}

void OpenRoomPermissions::onRoomPoked(const QString &roomId)
{
    // Only the open room is read; the answer replaces the current one, which
    // stays until it lands.
    if (!roomId.isEmpty() && roomId == m_roomId)
        request();
}
