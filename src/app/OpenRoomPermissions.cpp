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
}

OpenRoomPermissions::OpenRoomPermissions(QObject *parent) : QObject(parent) {}

void OpenRoomPermissions::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    m_op = 0;
    clearFacts();
    if (m_client) {
        connect(m_client, &MatrixClient::roomOwnPermissionsReceived, this,
                &OpenRoomPermissions::onReceived);
        connect(m_client, &MatrixClient::roomMemberEventSeen, this,
                &OpenRoomPermissions::onRoomPoked);
        connect(m_client, &MatrixClient::roomsChanged, this,
                &OpenRoomPermissions::onRoomsChanged);
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
    m_encryptionState = openRoomEncryptionState();
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
    m_moderation.clear();
}

QVariantMap OpenRoomPermissions::commandFacts() const
{
    if (!m_ownMembership.isEmpty() && m_ownMembership != QLatin1String("joined")) {
        QVariantMap none;
        for (const char *key :
             {"kick", "ban", "unban", "invite", "topic", "roomname"})
            none.insert(QLatin1String(key), false);
        return none;
    }
    return m_moderation;
}

void OpenRoomPermissions::onReceived(quint64 opId, const QString &roomId,
                                     const QVariantMap &facts)
{
    if (opId == 0 || opId != m_op || roomId != m_roomId)
        return;
    m_op = 0;
    const QString sendKey = QStringLiteral("canSendMessages");
    const QString redactKey = QStringLiteral("canRedactOthers");
    const QString membership =
        facts.value(QStringLiteral("ownMembership")).toString();
    const bool sendAnswered = facts.contains(sendKey);
    const bool canSend = !sendAnswered || facts.value(sendKey).toBool();
    const bool redactKnown = facts.contains(redactKey);
    const bool canRedact = redactKnown && facts.value(redactKey).toBool();
    static const struct { const char *key; const char *hint; } kModeration[] = {
        {"canKick", "kick"},         {"canBan", "ban"},
        {"canUnban", "unban"},       {"canInvite", "invite"},
        {"canEditTopic", "topic"},   {"canEditName", "roomname"},
    };
    QVariantMap moderation;
    for (const auto &m : kModeration) {
        if (facts.contains(QLatin1String(m.key)))
            moderation.insert(QLatin1String(m.hint),
                              facts.value(QLatin1String(m.key)).toBool());
    }
    if (membership == m_ownMembership && sendAnswered == m_sendAnswered
        && canSend == m_canSendMessages && redactKnown == m_redactKnown
        && canRedact == m_canRedactOthers && moderation == m_moderation) {
        return;
    }
    m_ownMembership = membership;
    m_sendAnswered = sendAnswered;
    m_canSendMessages = canSend;
    m_redactKnown = redactKnown;
    m_canRedactOthers = canRedact;
    m_moderation = moderation;
    Q_EMIT changed();
}

void OpenRoomPermissions::onRoomPoked(const QString &roomId)
{
    if (!roomId.isEmpty() && roomId == m_roomId)
        request();
}

int OpenRoomPermissions::openRoomEncryptionState() const
{
    if (!m_client || m_roomId.isEmpty())
        return 0;
    const RoomInfo room = m_client->roomInfo(m_roomId);
    if (room.id != m_roomId)
        return 0;
    return 1 | (room.encrypted ? 2 : 0) | (room.encryptionKnown ? 4 : 0);
}

void OpenRoomPermissions::onRoomsChanged()
{
    if (m_roomId.isEmpty() || openRoomEncryptionState() == m_encryptionState)
        return;
    request();
}
